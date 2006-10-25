/******************************************************
The B-tree

(c) 1994-1996 Innobase Oy

Created 6/2/1994 Heikki Tuuri
*******************************************************/

#include "btr0btr.h"

#ifdef UNIV_NONINL
#include "btr0btr.ic"
#endif

#include "fsp0fsp.h"
#include "page0page.h"
#include "page0zip.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "ibuf0ibuf.h"
#include "trx0trx.h"

/*
Latching strategy of the InnoDB B-tree
--------------------------------------
A tree latch protects all non-leaf nodes of the tree. Each node of a tree
also has a latch of its own.

A B-tree operation normally first acquires an S-latch on the tree. It
searches down the tree and releases the tree latch when it has the
leaf node latch. To save CPU time we do not acquire any latch on
non-leaf nodes of the tree during a search, those pages are only bufferfixed.

If an operation needs to restructure the tree, it acquires an X-latch on
the tree before searching to a leaf node. If it needs, for example, to
split a leaf,
(1) InnoDB decides the split point in the leaf,
(2) allocates a new page,
(3) inserts the appropriate node pointer to the first non-leaf level,
(4) releases the tree X-latch,
(5) and then moves records from the leaf to the new allocated page.

Node pointers
-------------
Leaf pages of a B-tree contain the index records stored in the
tree. On levels n > 0 we store 'node pointers' to pages on level
n - 1. For each page there is exactly one node pointer stored:
thus the our tree is an ordinary B-tree, not a B-link tree.

A node pointer contains a prefix P of an index record. The prefix
is long enough so that it determines an index record uniquely.
The file page number of the child page is added as the last
field. To the child page we can store node pointers or index records
which are >= P in the alphabetical order, but < P1 if there is
a next node pointer on the level, and P1 is its prefix.

If a node pointer with a prefix P points to a non-leaf child,
then the leftmost record in the child must have the same
prefix P. If it points to a leaf node, the child is not required
to contain any record with a prefix equal to P. The leaf case
is decided this way to allow arbitrary deletions in a leaf node
without touching upper levels of the tree.

We have predefined a special minimum record which we
define as the smallest record in any alphabetical order.
A minimum record is denoted by setting a bit in the record
header. A minimum record acts as the prefix of a node pointer
which points to a leftmost node on any level of the tree.

File page allocation
--------------------
In the root node of a B-tree there are two file segment headers.
The leaf pages of a tree are allocated from one file segment, to
make them consecutive on disk if possible. From the other file segment
we allocate pages for the non-leaf levels of the tree.
*/

/******************************************************************
Gets the root node of a tree and x-latches it. */
static
buf_block_t*
btr_root_block_get(
/*===============*/
				/* out: root page, x-latched */
	dict_index_t*	index,	/* in: index tree */
	mtr_t*		mtr)	/* in: mtr */
{
	ulint		space;
	ulint		root_page_no;
	buf_block_t*	block;

	space = dict_index_get_space(index);
	root_page_no = dict_index_get_page(index);

	block = btr_block_get(space, root_page_no, RW_X_LATCH, mtr);
	ut_a((ibool)!!page_is_comp(buf_block_get_frame(block))
	     == dict_table_is_comp(index->table));

	return(block);
}

/******************************************************************
Gets the root node of a tree and x-latches it. */

page_t*
btr_root_get(
/*=========*/
				/* out: root page, x-latched */
	dict_index_t*	index,	/* in: index tree */
	mtr_t*		mtr)	/* in: mtr */
{
	return(buf_block_get_frame(btr_root_block_get(index, mtr)));
}

/*****************************************************************
Gets pointer to the previous user record in the tree. It is assumed that
the caller has appropriate latches on the page and its neighbor. */

rec_t*
btr_get_prev_user_rec(
/*==================*/
			/* out: previous user record, NULL if there is none */
	rec_t*	rec,	/* in: record on leaf level */
	mtr_t*	mtr)	/* in: mtr holding a latch on the page, and if
			needed, also to the previous page */
{
	page_t*	page;
	page_t*	prev_page;
	ulint	prev_page_no;
	ulint	space;

	if (!page_rec_is_infimum(rec)) {

		rec_t*	prev_rec = page_rec_get_prev(rec);

		if (!page_rec_is_infimum(prev_rec)) {

			return(prev_rec);
		}
	}

	page = page_align(rec);
	prev_page_no = btr_page_get_prev(page, mtr);
	space = page_get_space_id(page);

	if (prev_page_no != FIL_NULL) {

		buf_block_t*	prev_block;

		prev_block = buf_page_get_with_no_latch(space, prev_page_no,
							mtr);
		prev_page = buf_block_get_frame(prev_block);
		/* The caller must already have a latch to the brother */
		ut_ad(mtr_memo_contains(mtr, prev_block,
					MTR_MEMO_PAGE_S_FIX)
		      || mtr_memo_contains(mtr, prev_block,
					   MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(prev_page) == page_is_comp(page));
		ut_a(btr_page_get_next(prev_page, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		return(page_rec_get_prev(page_get_supremum_rec(prev_page)));
	}

	return(NULL);
}

/*****************************************************************
Gets pointer to the next user record in the tree. It is assumed that the
caller has appropriate latches on the page and its neighbor. */

rec_t*
btr_get_next_user_rec(
/*==================*/
			/* out: next user record, NULL if there is none */
	rec_t*	rec,	/* in: record on leaf level */
	mtr_t*	mtr)	/* in: mtr holding a latch on the page, and if
			needed, also to the next page */
{
	page_t*	page;
	page_t*	next_page;
	ulint	next_page_no;
	ulint	space;

	if (!page_rec_is_supremum(rec)) {

		rec_t*	next_rec = page_rec_get_next(rec);

		if (!page_rec_is_supremum(next_rec)) {

			return(next_rec);
		}
	}

	page = page_align(rec);
	next_page_no = btr_page_get_next(page, mtr);
	space = page_get_space_id(page);

	if (next_page_no != FIL_NULL) {
		buf_block_t*	next_block;

		next_block = buf_page_get_with_no_latch(space, next_page_no,
							mtr);
		next_page = buf_block_get_frame(next_block);
		/* The caller must already have a latch to the brother */
		ut_ad(mtr_memo_contains(mtr, next_block, MTR_MEMO_PAGE_S_FIX)
		      || mtr_memo_contains(mtr, next_block,
					   MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(next_page) == page_is_comp(page));
		ut_a(btr_page_get_prev(next_page, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		return(page_rec_get_next(page_get_infimum_rec(next_page)));
	}

	return(NULL);
}

/******************************************************************
Creates a new index page (not the root, and also not
used in page reorganization). */
static
void
btr_page_create(
/*============*/
	buf_block_t*	block,	/* in/out: page to be created */
	page_zip_des_t*	page_zip,/* in/out: compressed page, or NULL */
	dict_index_t*	index,	/* in: index */
	ulint		level,	/* in: the B-tree level of the page */
	mtr_t*		mtr)	/* in: mtr */
{
	page_t*		page = buf_block_get_frame(block);

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));

	if (UNIV_LIKELY_NULL(page_zip)) {
		page_create_zip(block, index, level, mtr);
	} else {
		page_create(block, mtr, dict_table_is_comp(index->table));
		/* Set the level of the new index page */
		btr_page_set_level(page, NULL, level, mtr);
	}

	block->check_index_page_at_flush = TRUE;

	btr_page_set_index_id(page, page_zip, index->id, mtr);
}

/******************************************************************
Allocates a new file page to be used in an ibuf tree. Takes the page from
the free list of the tree, which must contain pages! */
static
buf_block_t*
btr_page_alloc_for_ibuf(
/*====================*/
				/* out: new allocated block, x-latched */
	dict_index_t*	index,	/* in: index tree */
	mtr_t*		mtr)	/* in: mtr */
{
	fil_addr_t	node_addr;
	page_t*		root;
	page_t*		new_page;
	buf_block_t*	new_block;

	root = btr_root_get(index, mtr);

	node_addr = flst_get_first(root + PAGE_HEADER
				   + PAGE_BTR_IBUF_FREE_LIST, mtr);
	ut_a(node_addr.page != FIL_NULL);

	new_block = buf_page_get(dict_index_get_space(index), node_addr.page,
				 RW_X_LATCH, mtr);
	new_page = buf_block_get_frame(new_block);
#ifdef UNIV_SYNC_DEBUG
	buf_block_dbg_add_level(new_block, SYNC_TREE_NODE_NEW);
#endif /* UNIV_SYNC_DEBUG */

	flst_remove(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
		    new_page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE,
		    mtr);
	ut_ad(flst_validate(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
			    mtr));

	return(new_block);
}

/******************************************************************
Allocates a new file page to be used in an index tree. NOTE: we assume
that the caller has made the reservation for free extents! */

buf_block_t*
btr_page_alloc(
/*===========*/
					/* out: new allocated block, x-latched;
					NULL if out of space */
	dict_index_t*	index,		/* in: index */
	ulint		hint_page_no,	/* in: hint of a good page */
	byte		file_direction,	/* in: direction where a possible
					page split is made */
	ulint		level,		/* in: level where the page is placed
					in the tree */
	mtr_t*		mtr)		/* in: mtr */
{
	fseg_header_t*	seg_header;
	page_t*		root;
	buf_block_t*	new_block;
	ulint		new_page_no;

	if (index->type & DICT_IBUF) {

		return(btr_page_alloc_for_ibuf(index, mtr));
	}

	root = btr_root_get(index, mtr);

	if (level == 0) {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
	} else {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;
	}

	/* Parameter TRUE below states that the caller has made the
	reservation for free extents, and thus we know that a page can
	be allocated: */

	new_page_no = fseg_alloc_free_page_general(seg_header, hint_page_no,
						   file_direction, TRUE, mtr);
	if (new_page_no == FIL_NULL) {

		return(NULL);
	}

	new_block = buf_page_get(dict_index_get_space(index), new_page_no,
				 RW_X_LATCH, mtr);
#ifdef UNIV_SYNC_DEBUG
	buf_block_dbg_add_level(new_block, SYNC_TREE_NODE_NEW);
#endif /* UNIV_SYNC_DEBUG */

	return(new_block);
}

/******************************************************************
Gets the number of pages in a B-tree. */

ulint
btr_get_size(
/*=========*/
				/* out: number of pages */
	dict_index_t*	index,	/* in: index */
	ulint		flag)	/* in: BTR_N_LEAF_PAGES or BTR_TOTAL_SIZE */
{
	fseg_header_t*	seg_header;
	page_t*		root;
	ulint		n;
	ulint		dummy;
	mtr_t		mtr;

	mtr_start(&mtr);

	mtr_s_lock(dict_index_get_lock(index), &mtr);

	root = btr_root_get(index, &mtr);

	if (flag == BTR_N_LEAF_PAGES) {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;

		fseg_n_reserved_pages(seg_header, &n, &mtr);

	} else if (flag == BTR_TOTAL_SIZE) {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

		n = fseg_n_reserved_pages(seg_header, &dummy, &mtr);

		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;

		n += fseg_n_reserved_pages(seg_header, &dummy, &mtr);
	} else {
		ut_error;
	}

	mtr_commit(&mtr);

	return(n);
}

/******************************************************************
Frees a page used in an ibuf tree. Puts the page to the free list of the
ibuf tree. */
static
void
btr_page_free_for_ibuf(
/*===================*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: block to be freed, x-latched */
	mtr_t*		mtr)	/* in: mtr */
{
	page_t*		root;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	root = btr_root_get(index, mtr);

	flst_add_first(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
		       buf_block_get_frame(block)
		       + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, mtr);

	ut_ad(flst_validate(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
			    mtr));
}

/******************************************************************
Frees a file page used in an index tree. Can be used also to (BLOB)
external storage pages, because the page level 0 can be given as an
argument. */

void
btr_page_free_low(
/*==============*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: block to be freed, x-latched */
	ulint		level,	/* in: page level */
	mtr_t*		mtr)	/* in: mtr */
{
	fseg_header_t*	seg_header;
	page_t*		root;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	/* The page gets invalid for optimistic searches: increment the frame
	modify clock */

	buf_block_modify_clock_inc(block);

	if (index->type & DICT_IBUF) {

		btr_page_free_for_ibuf(index, block, mtr);

		return;
	}

	root = btr_root_get(index, mtr);

	if (level == 0) {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
	} else {
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;
	}

	fseg_free_page(seg_header,
		       buf_block_get_space(block),
		       buf_block_get_page_no(block), mtr);
}

/******************************************************************
Frees a file page used in an index tree. NOTE: cannot free field external
storage pages because the page must contain info on its level. */

void
btr_page_free(
/*==========*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: block to be freed, x-latched */
	mtr_t*		mtr)	/* in: mtr */
{
	ulint		level;

	level = btr_page_get_level(buf_block_get_frame(block), mtr);

	btr_page_free_low(index, block, level, mtr);
}

/******************************************************************
Sets the child node file address in a node pointer. */
UNIV_INLINE
void
btr_node_ptr_set_child_page_no(
/*===========================*/
	rec_t*		rec,	/* in: node pointer record */
	page_zip_des_t*	page_zip,/* in/out: compressed page whose uncompressed
				part will be updated, or NULL */
	const ulint*	offsets,/* in: array returned by rec_get_offsets() */
	ulint		page_no,/* in: child node address */
	mtr_t*		mtr)	/* in: mtr */
{
	byte*	field;
	ulint	len;

	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(!page_is_leaf(page_align(rec)));
	ut_ad(!rec_offs_comp(offsets) || rec_get_node_ptr_flag(rec));

	/* The child address is in the last field */
	field = rec_get_nth_field(rec, offsets,
				  rec_offs_n_fields(offsets) - 1, &len);

	ut_ad(len == REC_NODE_PTR_SIZE);

	if (UNIV_LIKELY_NULL(page_zip)) {
		page_zip_write_node_ptr(page_zip, rec,
					rec_offs_data_size(offsets),
					page_no, mtr);
	} else {
		mlog_write_ulint(field, page_no, MLOG_4BYTES, mtr);
	}
}

/****************************************************************
Returns the child page of a node pointer and x-latches it. */
static
buf_block_t*
btr_node_ptr_get_child(
/*===================*/
				/* out: child page, x-latched */
	const rec_t*	node_ptr,/* in: node pointer */
	const ulint*	offsets,/* in: array returned by rec_get_offsets() */
	mtr_t*		mtr)	/* in: mtr */
{
	ulint	page_no;
	ulint	space;

	ut_ad(rec_offs_validate(node_ptr, NULL, offsets));
	space = page_get_space_id(page_align((rec_t*) node_ptr));
	page_no = btr_node_ptr_get_child_page_no(node_ptr, offsets);

	return(btr_block_get(space, page_no, RW_X_LATCH, mtr));
}

/****************************************************************
Returns the upper level node pointer to a page. It is assumed that mtr holds
an x-latch on the tree. */
static
ulint*
btr_page_get_father_node_ptr(
/*=========================*/
				/* out: rec_get_offsets() of the
				node pointer record */
	ulint*		offsets,/* in: work area for the return value */
	mem_heap_t*	heap,	/* in: memory heap to use */
	btr_cur_t*	cursor,	/* in: cursor pointing to user record,
				out: cursor on node pointer record,
				its page x-latched */
	mtr_t*		mtr)	/* in: mtr */
{
	dtuple_t*	tuple;
	rec_t*		user_rec;
	rec_t*		node_ptr;
	ulint		level;
	ulint		page_no;
	dict_index_t*	index;

	page_no = buf_block_get_page_no(btr_cur_get_block(cursor));
	index = btr_cur_get_index(cursor);

	ut_ad(mtr_memo_contains(mtr, dict_index_get_lock(index),
				MTR_MEMO_X_LOCK));

	ut_ad(dict_index_get_page(index) != page_no);

	level = btr_page_get_level(btr_cur_get_page(cursor), mtr);
	user_rec = btr_cur_get_rec(cursor);
	ut_a(page_rec_is_user_rec(user_rec));
	tuple = dict_index_build_node_ptr(index, user_rec, 0, heap, level);

	btr_cur_search_to_nth_level(index, level + 1, tuple, PAGE_CUR_LE,
				    BTR_CONT_MODIFY_TREE, cursor, 0, mtr);

	node_ptr = btr_cur_get_rec(cursor);
	ut_ad(!page_rec_is_comp(node_ptr)
	      || rec_get_status(node_ptr) == REC_STATUS_NODE_PTR);
	offsets = rec_get_offsets(node_ptr, index, offsets,
				  ULINT_UNDEFINED, &heap);

	if (UNIV_UNLIKELY(btr_node_ptr_get_child_page_no(node_ptr, offsets)
			  != page_no)) {
		rec_t*	print_rec;
		fputs("InnoDB: Dump of the child page:\n", stderr);
		buf_page_print(page_align(user_rec), 0);
		fputs("InnoDB: Dump of the parent page:\n", stderr);
		buf_page_print(page_align(node_ptr), 0);

		fputs("InnoDB: Corruption of an index tree: table ", stderr);
		ut_print_name(stderr, NULL, TRUE, index->table_name);
		fputs(", index ", stderr);
		ut_print_name(stderr, NULL, FALSE, index->name);
		fprintf(stderr, ",\n"
			"InnoDB: father ptr page no %lu, child page no %lu\n",
			(ulong)
			btr_node_ptr_get_child_page_no(node_ptr, offsets),
			(ulong) page_no);
		print_rec = page_rec_get_next(
			page_get_infimum_rec(page_align(user_rec)));
		offsets = rec_get_offsets(print_rec, index,
					  offsets, ULINT_UNDEFINED, &heap);
		page_rec_print(print_rec, offsets);
		offsets = rec_get_offsets(node_ptr, index, offsets,
					  ULINT_UNDEFINED, &heap);
		page_rec_print(node_ptr, offsets);

		fputs("InnoDB: You should dump + drop + reimport the table"
		      " to fix the\n"
		      "InnoDB: corruption. If the crash happens at "
		      "the database startup, see\n"
		      "InnoDB: http://dev.mysql.com/doc/refman/5.1/en/"
		      "forcing-recovery.html about\n"
		      "InnoDB: forcing recovery. "
		      "Then dump + drop + reimport.\n", stderr);

		ut_error;
	}

	return(offsets);
}

/****************************************************************
Returns the upper level node pointer to a page. It is assumed that mtr holds
an x-latch on the tree. */
static
ulint*
btr_page_get_father_block(
/*======================*/
				/* out: rec_get_offsets() of the
				node pointer record */
	ulint*		offsets,/* in: work area for the return value */
	mem_heap_t*	heap,	/* in: memory heap to use */
	dict_index_t*	index,	/* in: b-tree index */
	buf_block_t*	block,	/* in: child page in the index */
	mtr_t*		mtr,	/* in: mtr */
	btr_cur_t*	cursor)	/* out: cursor on node pointer record,
				its page x-latched */
{
	rec_t*	rec
		= page_rec_get_next(page_get_infimum_rec(buf_block_get_frame(
								 block)));
	btr_cur_position(index, rec, block, cursor);
	return(btr_page_get_father_node_ptr(offsets, heap, cursor, mtr));
}

/****************************************************************
Seeks to the upper level node pointer to a page.
It is assumed that mtr holds an x-latch on the tree. */
static
void
btr_page_get_father(
/*================*/
	dict_index_t*	index,	/* in: b-tree index */
	buf_block_t*	block,	/* in: child page in the index */
	mtr_t*		mtr,	/* in: mtr */
	btr_cur_t*	cursor)	/* out: cursor on node pointer record,
				its page x-latched */
{
	mem_heap_t*	heap;
	rec_t*		rec
		= page_rec_get_next(page_get_infimum_rec(buf_block_get_frame(
								 block)));
	btr_cur_position(index, rec, block, cursor);

	heap = mem_heap_create(100);
	btr_page_get_father_node_ptr(NULL, heap, cursor, mtr);
	mem_heap_free(heap);
}

/****************************************************************
Creates the root node for a new index tree. */

ulint
btr_create(
/*=======*/
				/* out: page number of the created root,
				FIL_NULL if did not succeed */
	ulint		type,	/* in: type of the index */
	ulint		space,	/* in: space where created */
	dulint		index_id,/* in: index id */
	dict_index_t*	index,	/* in: index */
	mtr_t*		mtr)	/* in: mini-transaction handle */
{
	ulint		page_no;
	buf_block_t*	block;
	buf_frame_t*	frame;
	page_t*		page;
	page_zip_des_t*	page_zip;

	/* Create the two new segments (one, in the case of an ibuf tree) for
	the index tree; the segment headers are put on the allocated root page
	(for an ibuf tree, not in the root, but on a separate ibuf header
	page) */

	if (type & DICT_IBUF) {
		/* Allocate first the ibuf header page */
		buf_block_t*	ibuf_hdr_block = fseg_create(
			space, 0, IBUF_HEADER + IBUF_TREE_SEG_HEADER, mtr);

#ifdef UNIV_SYNC_DEBUG
		buf_block_dbg_add_level(ibuf_hdr_block, SYNC_TREE_NODE_NEW);
#endif /* UNIV_SYNC_DEBUG */
		ut_ad(buf_block_get_page_no(ibuf_hdr_block)
		      == IBUF_HEADER_PAGE_NO);
		/* Allocate then the next page to the segment: it will be the
		tree root page */

		page_no = fseg_alloc_free_page(buf_block_get_frame(
						       ibuf_hdr_block)
					       + IBUF_HEADER
					       + IBUF_TREE_SEG_HEADER,
					       IBUF_TREE_ROOT_PAGE_NO,
					       FSP_UP, mtr);
		ut_ad(page_no == IBUF_TREE_ROOT_PAGE_NO);

		block = buf_page_get(space, page_no, RW_X_LATCH, mtr);
	} else {
		block = fseg_create(space, 0, PAGE_HEADER + PAGE_BTR_SEG_TOP,
				    mtr);
	}

	if (block == NULL) {

		return(FIL_NULL);
	}

	page_no = buf_block_get_page_no(block);
	frame = buf_block_get_frame(block);

#ifdef UNIV_SYNC_DEBUG
	buf_block_dbg_add_level(block, SYNC_TREE_NODE_NEW);
#endif /* UNIV_SYNC_DEBUG */

	if (type & DICT_IBUF) {
		/* It is an insert buffer tree: initialize the free list */

		ut_ad(page_no == IBUF_TREE_ROOT_PAGE_NO);

		flst_init(frame + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr);
	} else {
		/* It is a non-ibuf tree: create a file segment for leaf
		pages */
		fseg_create(space, page_no, PAGE_HEADER + PAGE_BTR_SEG_LEAF,
			    mtr);
		/* The fseg create acquires a second latch on the page,
		therefore we must declare it: */
#ifdef UNIV_SYNC_DEBUG
		buf_block_dbg_add_level(block, SYNC_TREE_NODE_NEW);
#endif /* UNIV_SYNC_DEBUG */
	}

	/* Create a new index page on the the allocated segment page */
	page_zip = buf_block_get_page_zip(block);

	if (UNIV_LIKELY_NULL(page_zip)) {
		page = page_create_zip(block, index, 0, mtr);
	} else {
		page = page_create(block, mtr,
				   dict_table_is_comp(index->table));
		/* Set the level of the new index page */
		btr_page_set_level(page, NULL, 0, mtr);
	}

	block->check_index_page_at_flush = TRUE;

	/* Set the index id of the page */
	btr_page_set_index_id(page, page_zip, index_id, mtr);

	/* Set the next node and previous node fields */
	btr_page_set_next(page, page_zip, FIL_NULL, mtr);
	btr_page_set_prev(page, page_zip, FIL_NULL, mtr);

	/* We reset the free bits for the page to allow creation of several
	trees in the same mtr, otherwise the latch on a bitmap page would
	prevent it because of the latching order */

	ibuf_reset_free_bits_with_type(type, block);

	/* In the following assertion we test that two records of maximum
	allowed size fit on the root page: this fact is needed to ensure
	correctness of split algorithms */

	ut_ad(page_get_max_insert_size(page, 2) > 2 * BTR_PAGE_MAX_REC_SIZE);

	return(page_no);
}

/****************************************************************
Frees a B-tree except the root page, which MUST be freed after this
by calling btr_free_root. */

void
btr_free_but_not_root(
/*==================*/
	ulint	space,		/* in: space where created */
	ulint	root_page_no)	/* in: root page number */
{
	ibool	finished;
	page_t*	root;
	mtr_t	mtr;

leaf_loop:
	mtr_start(&mtr);

	root = btr_page_get(space, root_page_no, RW_X_LATCH, &mtr);

	/* NOTE: page hash indexes are dropped when a page is freed inside
	fsp0fsp. */

	finished = fseg_free_step(root + PAGE_HEADER + PAGE_BTR_SEG_LEAF,
				  &mtr);
	mtr_commit(&mtr);

	if (!finished) {

		goto leaf_loop;
	}
top_loop:
	mtr_start(&mtr);

	root = btr_page_get(space, root_page_no, RW_X_LATCH, &mtr);

	finished = fseg_free_step_not_header(
		root + PAGE_HEADER + PAGE_BTR_SEG_TOP, &mtr);
	mtr_commit(&mtr);

	if (!finished) {

		goto top_loop;
	}
}

/****************************************************************
Frees the B-tree root page. Other tree MUST already have been freed. */

void
btr_free_root(
/*==========*/
	ulint	space,		/* in: space where created */
	ulint	root_page_no,	/* in: root page number */
	mtr_t*	mtr)		/* in: a mini-transaction which has already
				been started */
{
	buf_block_t*	block;
	fseg_header_t*	header;

	block = btr_block_get(space, root_page_no, RW_X_LATCH, mtr);

	btr_search_drop_page_hash_index(block);

	header = buf_block_get_frame(block) + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	while (!fseg_free_step(header, mtr));
}

/*****************************************************************
Reorganizes an index page. */
static
ibool
btr_page_reorganize_low(
/*====================*/
	ibool		recovery,/* in: TRUE if called in recovery:
				locks should not be updated, i.e.,
				there cannot exist locks on the
				page, and a hash index should not be
				dropped: it cannot exist */
	buf_block_t*	block,	/* in: page to be reorganized */
	dict_index_t*	index,	/* in: record descriptor */
	mtr_t*		mtr)	/* in: mtr */
{
	page_t*		page		= buf_block_get_frame(block);
	page_zip_des_t*	page_zip	= buf_block_get_page_zip(block);
	buf_block_t*	temp_block;
	page_t*		temp_page;
	ulint		log_mode;
	ulint		data_size1;
	ulint		data_size2;
	ulint		max_ins_size1;
	ulint		max_ins_size2;
	ibool		success		= FALSE;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!!page_is_comp(page) == dict_table_is_comp(index->table));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page));
#endif /* UNIV_ZIP_DEBUG */
	data_size1 = page_get_data_size(page);
	max_ins_size1 = page_get_max_insert_size_after_reorganize(page, 1);

	/* Write the log record */
	mlog_open_and_write_index(mtr, page, index, page_is_comp(page)
				  ? MLOG_COMP_PAGE_REORGANIZE
				  : MLOG_PAGE_REORGANIZE, 0);

	/* Turn logging off */
	log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);

	temp_block = buf_block_alloc(0);
	temp_page = temp_block->frame;

	/* Copy the old page to temporary space */
	buf_frame_copy(temp_page, page);

	if (UNIV_LIKELY(!recovery)) {
		btr_search_drop_page_hash_index(block);
	}

	/* Recreate the page: note that global data on page (possible
	segment headers, next page-field, etc.) is preserved intact */

	page_create(block, mtr, dict_table_is_comp(index->table));
	block->check_index_page_at_flush = TRUE;

	/* Copy the records from the temporary space to the recreated page;
	do not copy the lock bits yet */

	page_copy_rec_list_end_no_locks(block, temp_block,
					page_get_infimum_rec(temp_page),
					index, mtr);
	/* Copy max trx id to recreated page */
	page_set_max_trx_id(block, NULL, page_get_max_trx_id(temp_page));

	if (UNIV_LIKELY_NULL(page_zip)
	    && UNIV_UNLIKELY
	    (!page_zip_compress(page_zip, page, index, NULL))) {

		/* Restore the old page and exit. */
		buf_frame_copy(page, temp_page);

		goto func_exit;
	}

	if (UNIV_LIKELY(!recovery)) {
		/* Update the record lock bitmaps */
		lock_move_reorganize_page(block, temp_block);
	}

	data_size2 = page_get_data_size(page);
	max_ins_size2 = page_get_max_insert_size_after_reorganize(page, 1);

	if (UNIV_UNLIKELY(data_size1 != data_size2)
	    || UNIV_UNLIKELY(max_ins_size1 != max_ins_size2)) {
		buf_page_print(page, 0);
		buf_page_print(temp_page, 0);
		fprintf(stderr,
			"InnoDB: Error: page old data size %lu"
			" new data size %lu\n"
			"InnoDB: Error: page old max ins size %lu"
			" new max ins size %lu\n"
			"InnoDB: Submit a detailed bug report"
			" to http://bugs.mysql.com\n",
			(unsigned long) data_size1, (unsigned long) data_size2,
			(unsigned long) max_ins_size1,
			(unsigned long) max_ins_size2);
	} else {
		success = TRUE;
	}

func_exit:
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page));
#endif /* UNIV_ZIP_DEBUG */
	buf_block_free(temp_block);

	/* Restore logging mode */
	mtr_set_log_mode(mtr, log_mode);

	return(success);
}

/*****************************************************************
Reorganizes an index page. */

ibool
btr_page_reorganize(
/*================*/
				/* out: TRUE on success, FALSE on failure */
	buf_block_t*	block,	/* in: page to be reorganized */
	dict_index_t*	index,	/* in: record descriptor */
	mtr_t*		mtr)	/* in: mtr */
{
	return(btr_page_reorganize_low(FALSE, block, index, mtr));
}

/***************************************************************
Parses a redo log record of reorganizing a page. */

byte*
btr_parse_page_reorganize(
/*======================*/
				/* out: end of log record or NULL */
	byte*		ptr,	/* in: buffer */
	byte*		end_ptr __attribute__((unused)),
				/* in: buffer end */
	dict_index_t*	index,	/* in: record descriptor */
	buf_block_t*	block,	/* in: page to be reorganized, or NULL */
	mtr_t*		mtr)	/* in: mtr or NULL */
{
	ut_ad(ptr && end_ptr);

	/* The record is empty, except for the record initial part */

	if (UNIV_LIKELY(block != NULL)) {
		btr_page_reorganize_low(TRUE, block, index, mtr);
	}

	return(ptr);
}

/*****************************************************************
Empties an index page. */
static
void
btr_page_empty(
/*===========*/
	buf_block_t*	block,	/* in: page to be emptied */
	page_zip_des_t*	page_zip,/* out: compressed page, or NULL */
	mtr_t*		mtr,	/* in: mtr */
	dict_index_t*	index)	/* in: index of the page */
{
	page_t*	page = buf_block_get_frame(block);

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page));
#endif /* UNIV_ZIP_DEBUG */

	btr_search_drop_page_hash_index(block);

	/* Recreate the page: note that global data on page (possible
	segment headers, next page-field, etc.) is preserved intact */

	if (UNIV_LIKELY_NULL(page_zip)) {
		page_create_zip(block, index,
				btr_page_get_level(page, mtr), mtr);
	} else {
		page_create(block, mtr, dict_table_is_comp(index->table));
	}

	block->check_index_page_at_flush = TRUE;
}

/*****************************************************************
Makes tree one level higher by splitting the root, and inserts
the tuple. It is assumed that mtr contains an x-latch on the tree.
NOTE that the operation of this function must always succeed,
we cannot reverse it: therefore enough free disk space must be
guaranteed to be available before this function is called. */

rec_t*
btr_root_raise_and_insert(
/*======================*/
				/* out: inserted record */
	btr_cur_t*	cursor,	/* in: cursor at which to insert: must be
				on the root page; when the function returns,
				the cursor is positioned on the predecessor
				of the inserted record */
	dtuple_t*	tuple,	/* in: tuple to insert */
	const ulint*	ext,	/* in: array of extern field numbers */
	ulint		n_ext,	/* in: number of elements in vec */
	mtr_t*		mtr)	/* in: mtr */
{
	dict_index_t*	index;
	page_t*		root;
	page_t*		new_page;
	ulint		new_page_no;
	rec_t*		rec;
	mem_heap_t*	heap;
	dtuple_t*	node_ptr;
	ulint		level;
	rec_t*		node_ptr_rec;
	page_cur_t*	page_cursor;
	page_zip_des_t*	root_page_zip;
	page_zip_des_t*	new_page_zip;
	buf_block_t*	root_block;
	buf_block_t*	new_block;

	root = btr_cur_get_page(cursor);
	root_block = btr_cur_get_block(cursor);
	root_page_zip = buf_block_get_page_zip(root_block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!root_page_zip || page_zip_validate(root_page_zip, root));
#endif /* UNIV_ZIP_DEBUG */
	index = btr_cur_get_index(cursor);

	ut_ad(dict_index_get_page(index) == page_get_page_no(root));
	ut_ad(mtr_memo_contains(mtr, dict_index_get_lock(index),
				MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, root_block, MTR_MEMO_PAGE_X_FIX));
	btr_search_drop_page_hash_index(root_block);

	/* Allocate a new page to the tree. Root splitting is done by first
	moving the root records to the new page, emptying the root, putting
	a node pointer to the new page, and then splitting the new page. */

	level = btr_page_get_level(root, mtr);

	new_block = btr_page_alloc(index, 0, FSP_NO_DIR, level, mtr);
	new_page = buf_block_get_frame(new_block);
	new_page_zip = buf_block_get_page_zip(new_block);
	ut_a(!new_page_zip == !root_page_zip);
	ut_a(!new_page_zip || new_page_zip->size == root_page_zip->size);

	btr_page_create(new_block, new_page_zip, index, level, mtr);

	/* Set the next node and previous node fields of new page */
	btr_page_set_next(new_page, new_page_zip, FIL_NULL, mtr);
	btr_page_set_prev(new_page, new_page_zip, FIL_NULL, mtr);

	/* Copy the records from root to the new page one by one. */

	if (UNIV_UNLIKELY
	    (!page_copy_rec_list_end(new_block, root_block,
				     page_get_infimum_rec(root),
				     index, mtr))) {
		ut_a(new_page_zip);

		/* Copy the page byte for byte. */
		page_zip_copy(new_page_zip, new_page,
			      root_page_zip, root, index, mtr);
	}

	/* If this is a pessimistic insert which is actually done to
	perform a pessimistic update then we have stored the lock
	information of the record to be inserted on the infimum of the
	root page: we cannot discard the lock structs on the root page */

	lock_update_root_raise(new_block, root_block);

	/* Create a memory heap where the node pointer is stored */
	heap = mem_heap_create(100);

	rec = page_rec_get_next(page_get_infimum_rec(new_page));
	new_page_no = buf_block_get_page_no(new_block);

	/* Build the node pointer (= node key and page address) for the
	child */

	node_ptr = dict_index_build_node_ptr(index, rec, new_page_no, heap,
					     level);
	/* The node pointer must be marked as the predefined minimum record,
	as there is no lower alphabetical limit to records in the leftmost
	node of a level: */
	dtuple_set_info_bits(node_ptr,
			     dtuple_get_info_bits(node_ptr)
			     | REC_INFO_MIN_REC_FLAG);

	/* Rebuild the root page to get free space */
	if (UNIV_LIKELY_NULL(root_page_zip)) {
		page_create_zip(root_block, index, level + 1, mtr);
	} else {
		page_create(root_block, mtr, dict_table_is_comp(index->table));
		btr_page_set_level(root, NULL, level + 1, mtr);
	}

	/* Set the next node and previous node fields, although
	they should already have been set.  The previous node field
	must be FIL_NULL if root_page_zip != NULL, because the
	REC_INFO_MIN_REC_FLAG (of the first user record) will be
	set if and only if btr_page_get_prev() == FIL_NULL. */
	btr_page_set_next(root, root_page_zip, FIL_NULL, mtr);
	btr_page_set_prev(root, root_page_zip, FIL_NULL, mtr);

	root_block->check_index_page_at_flush = TRUE;

	page_cursor = btr_cur_get_page_cur(cursor);

	/* Insert node pointer to the root */

	page_cur_set_before_first(root_block, page_cursor);

	node_ptr_rec = page_cur_tuple_insert(page_cursor, node_ptr,
					     index, NULL, 0, mtr);

	/* The root page should only contain the node pointer
	to new_page at this point.  Thus, the data should fit. */
	ut_a(node_ptr_rec);

	/* Free the memory heap */
	mem_heap_free(heap);

	/* We play safe and reset the free bits for the new page */

#if 0
	fprintf(stderr, "Root raise new page no %lu\n", new_page_no);
#endif

	ibuf_reset_free_bits_with_type(index->type, new_block);
	/* Reposition the cursor to the child node */
	page_cur_search(new_block, index, tuple,
			PAGE_CUR_LE, page_cursor);

	/* Split the child and insert tuple */
	return(btr_page_split_and_insert(cursor, tuple, ext, n_ext, mtr));
}

/*****************************************************************
Decides if the page should be split at the convergence point of inserts
converging to the left. */

ibool
btr_page_get_split_rec_to_left(
/*===========================*/
				/* out: TRUE if split recommended */
	btr_cur_t*	cursor,	/* in: cursor at which to insert */
	rec_t**		split_rec) /* out: if split recommended,
				the first record on upper half page,
				or NULL if tuple to be inserted should
				be first */
{
	page_t*	page;
	rec_t*	insert_point;
	rec_t*	infimum;

	page = btr_cur_get_page(cursor);
	insert_point = btr_cur_get_rec(cursor);

	if (page_header_get_ptr(page, PAGE_LAST_INSERT)
	    == page_rec_get_next(insert_point)) {

		infimum = page_get_infimum_rec(page);

		/* If the convergence is in the middle of a page, include also
		the record immediately before the new insert to the upper
		page. Otherwise, we could repeatedly move from page to page
		lots of records smaller than the convergence point. */

		if (infimum != insert_point
		    && page_rec_get_next(infimum) != insert_point) {

			*split_rec = insert_point;
		} else {
			*split_rec = page_rec_get_next(insert_point);
		}

		return(TRUE);
	}

	return(FALSE);
}

/*****************************************************************
Decides if the page should be split at the convergence point of inserts
converging to the right. */

ibool
btr_page_get_split_rec_to_right(
/*============================*/
				/* out: TRUE if split recommended */
	btr_cur_t*	cursor,	/* in: cursor at which to insert */
	rec_t**		split_rec) /* out: if split recommended,
				the first record on upper half page,
				or NULL if tuple to be inserted should
				be first */
{
	page_t*	page;
	rec_t*	insert_point;

	page = btr_cur_get_page(cursor);
	insert_point = btr_cur_get_rec(cursor);

	/* We use eager heuristics: if the new insert would be right after
	the previous insert on the same page, we assume that there is a
	pattern of sequential inserts here. */

	if (UNIV_LIKELY(page_header_get_ptr(page, PAGE_LAST_INSERT)
			== insert_point)) {

		rec_t*	next_rec;

		next_rec = page_rec_get_next(insert_point);

		if (page_rec_is_supremum(next_rec)) {
split_at_new:
			/* Split at the new record to insert */
			*split_rec = NULL;
		} else {
			rec_t*	next_next_rec = page_rec_get_next(next_rec);
			if (page_rec_is_supremum(next_next_rec)) {

				goto split_at_new;
			}

			/* If there are >= 2 user records up from the insert
			point, split all but 1 off. We want to keep one because
			then sequential inserts can use the adaptive hash
			index, as they can do the necessary checks of the right
			search position just by looking at the records on this
			page. */

			*split_rec = next_next_rec;
		}

		return(TRUE);
	}

	return(FALSE);
}

/*****************************************************************
Calculates a split record such that the tuple will certainly fit on
its half-page when the split is performed. We assume in this function
only that the cursor page has at least one user record. */
static
rec_t*
btr_page_get_sure_split_rec(
/*========================*/
					/* out: split record, or NULL if
					tuple will be the first record on
					upper half-page */
	btr_cur_t*	cursor,		/* in: cursor at which insert
					should be made */
	dtuple_t*	tuple,		/* in: tuple to insert */
	const ulint*	ext,		/* in: array of extern field numbers */
	ulint		n_ext)		/* in: number of elements in ext */
{
	page_t*		page;
	page_zip_des_t*	page_zip;
	ulint		insert_size;
	ulint		free_space;
	ulint		total_data;
	ulint		total_n_recs;
	ulint		total_space;
	ulint		incl_data;
	rec_t*		ins_rec;
	rec_t*		rec;
	rec_t*		next_rec;
	ulint		n;
	mem_heap_t*	heap;
	ulint*		offsets;

	page = btr_cur_get_page(cursor);

	insert_size = rec_get_converted_size(cursor->index, tuple, ext, n_ext);
	free_space  = page_get_free_space_of_empty(page_is_comp(page));

	page_zip = btr_cur_get_page_zip(cursor);
	if (UNIV_LIKELY_NULL(page_zip)) {
		/* Estimate the free space of an empty compressed page. */
		ulint	free_space_zip = page_zip_empty_size(
			cursor->index->n_fields, page_zip->size);

		if (UNIV_LIKELY(free_space > (ulint) free_space_zip)) {
			free_space = (ulint) free_space_zip;
			ut_a(insert_size <= free_space);
		}
	}

	/* free_space is now the free space of a created new page */

	total_data   = page_get_data_size(page) + insert_size;
	total_n_recs = page_get_n_recs(page) + 1;
	ut_ad(total_n_recs >= 2);
	total_space  = total_data + page_dir_calc_reserved_space(total_n_recs);

	n = 0;
	incl_data = 0;
	ins_rec = btr_cur_get_rec(cursor);
	rec = page_get_infimum_rec(page);

	heap = NULL;
	offsets = NULL;

	/* We start to include records to the left half, and when the
	space reserved by them exceeds half of total_space, then if
	the included records fit on the left page, they will be put there
	if something was left over also for the right page,
	otherwise the last included record will be the first on the right
	half page */

	do {
		/* Decide the next record to include */
		if (rec == ins_rec) {
			rec = NULL;	/* NULL denotes that tuple is
					now included */
		} else if (rec == NULL) {
			rec = page_rec_get_next(ins_rec);
		} else {
			rec = page_rec_get_next(rec);
		}

		if (rec == NULL) {
			/* Include tuple */
			incl_data += insert_size;
		} else {
			offsets = rec_get_offsets(rec, cursor->index,
						  offsets, ULINT_UNDEFINED,
						  &heap);
			incl_data += rec_offs_size(offsets);
		}

		n++;
	} while (incl_data + page_dir_calc_reserved_space(n)
		 < total_space / 2);

	if (incl_data + page_dir_calc_reserved_space(n) <= free_space) {
		/* The next record will be the first on
		the right half page if it is not the
		supremum record of page */

		if (rec == ins_rec) {
			rec = NULL;

			goto func_exit;
		} else if (rec == NULL) {
			next_rec = page_rec_get_next(ins_rec);
		} else {
			next_rec = page_rec_get_next(rec);
		}
		ut_ad(next_rec);
		if (!page_rec_is_supremum(next_rec)) {
			rec = next_rec;
		}
	}

func_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(rec);
}

/*****************************************************************
Returns TRUE if the insert fits on the appropriate half-page with the
chosen split_rec. */
static
ibool
btr_page_insert_fits(
/*=================*/
					/* out: TRUE if fits */
	btr_cur_t*	cursor,		/* in: cursor at which insert
					should be made */
	rec_t*		split_rec,	/* in: suggestion for first record
					on upper half-page, or NULL if
					tuple to be inserted should be first */
	const ulint*	offsets,	/* in: rec_get_offsets(
					split_rec, cursor->index) */
	dtuple_t*	tuple,		/* in: tuple to insert */
	const ulint*	ext,		/* in: array of extern field numbers */
	ulint		n_ext,		/* in: number of elements in ext */
	mem_heap_t*	heap)		/* in: temporary memory heap */
{
	page_t*	page;
	ulint	insert_size;
	ulint	free_space;
	ulint	total_data;
	ulint	total_n_recs;
	rec_t*	rec;
	rec_t*	end_rec;
	ulint*	offs;

	page = btr_cur_get_page(cursor);

	ut_ad(!split_rec == !offsets);
	ut_ad(!offsets
	      || !page_is_comp(page) == !rec_offs_comp(offsets));
	ut_ad(!offsets
	      || rec_offs_validate(split_rec, cursor->index, offsets));

	insert_size = rec_get_converted_size(cursor->index, tuple, ext, n_ext);
	free_space  = page_get_free_space_of_empty(page_is_comp(page));

	/* free_space is now the free space of a created new page */

	total_data   = page_get_data_size(page) + insert_size;
	total_n_recs = page_get_n_recs(page) + 1;

	/* We determine which records (from rec to end_rec, not including
	end_rec) will end up on the other half page from tuple when it is
	inserted. */

	if (split_rec == NULL) {
		rec = page_rec_get_next(page_get_infimum_rec(page));
		end_rec = page_rec_get_next(btr_cur_get_rec(cursor));

	} else if (cmp_dtuple_rec(tuple, split_rec, offsets) >= 0) {

		rec = page_rec_get_next(page_get_infimum_rec(page));
		end_rec = split_rec;
	} else {
		rec = split_rec;
		end_rec = page_get_supremum_rec(page);
	}

	if (total_data + page_dir_calc_reserved_space(total_n_recs)
	    <= free_space) {

		/* Ok, there will be enough available space on the
		half page where the tuple is inserted */

		return(TRUE);
	}

	offs = NULL;

	while (rec != end_rec) {
		/* In this loop we calculate the amount of reserved
		space after rec is removed from page. */

		offs = rec_get_offsets(rec, cursor->index, offs,
				       ULINT_UNDEFINED, &heap);

		total_data -= rec_offs_size(offs);
		total_n_recs--;

		if (total_data + page_dir_calc_reserved_space(total_n_recs)
		    <= free_space) {

			/* Ok, there will be enough available space on the
			half page where the tuple is inserted */

			return(TRUE);
		}

		rec = page_rec_get_next(rec);
	}

	return(FALSE);
}

/***********************************************************
Inserts a data tuple to a tree on a non-leaf level. It is assumed
that mtr holds an x-latch on the tree. */

void
btr_insert_on_non_leaf_level(
/*=========================*/
	dict_index_t*	index,	/* in: index */
	ulint		level,	/* in: level, must be > 0 */
	dtuple_t*	tuple,	/* in: the record to be inserted */
	mtr_t*		mtr)	/* in: mtr */
{
	big_rec_t*	dummy_big_rec;
	btr_cur_t	cursor;
	ulint		err;
	rec_t*		rec;

	ut_ad(level > 0);

	btr_cur_search_to_nth_level(index, level, tuple, PAGE_CUR_LE,
				    BTR_CONT_MODIFY_TREE,
				    &cursor, 0, mtr);

	err = btr_cur_pessimistic_insert(BTR_NO_LOCKING_FLAG
					 | BTR_KEEP_SYS_FLAG
					 | BTR_NO_UNDO_LOG_FLAG,
					 &cursor, tuple, &rec,
					 &dummy_big_rec, NULL, 0, NULL, mtr);
	ut_a(err == DB_SUCCESS);
}

/******************************************************************
Attaches the halves of an index page on the appropriate level in an
index tree. */
static
void
btr_attach_half_pages(
/*==================*/
	dict_index_t*	index,		/* in: the index tree */
	buf_block_t*	block,		/* in/out: page to be split */
	rec_t*		split_rec,	/* in: first record on upper
					half page */
	buf_block_t*	new_block,	/* in/out: the new half page */
	ulint		direction,	/* in: FSP_UP or FSP_DOWN */
	mtr_t*		mtr)		/* in: mtr */
{
	ulint		space;
	ulint		prev_page_no;
	ulint		next_page_no;
	ulint		level;
	page_t*		page		= buf_block_get_frame(block);
	page_t*		lower_page;
	page_t*		upper_page;
	ulint		lower_page_no;
	ulint		upper_page_no;
	page_zip_des_t*	lower_page_zip;
	page_zip_des_t*	upper_page_zip;
	dtuple_t*	node_ptr_upper;
	mem_heap_t*	heap;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr_memo_contains(mtr, new_block, MTR_MEMO_PAGE_X_FIX));

	/* Create a memory heap where the data tuple is stored */
	heap = mem_heap_create(1024);

	/* Based on split direction, decide upper and lower pages */
	if (direction == FSP_DOWN) {

		btr_cur_t	cursor;
		ulint*		offsets;

		lower_page = buf_block_get_frame(new_block);
		lower_page_no = buf_block_get_page_no(new_block);
		lower_page_zip = buf_block_get_page_zip(new_block);
		upper_page = buf_block_get_frame(block);
		upper_page_no = buf_block_get_page_no(block);
		upper_page_zip = buf_block_get_page_zip(block);

		/* Look up the index for the node pointer to page */
		offsets = btr_page_get_father_block(NULL, heap, index,
						    block, mtr, &cursor);

		/* Replace the address of the old child node (= page) with the
		address of the new lower half */

		btr_node_ptr_set_child_page_no(
			btr_cur_get_rec(&cursor),
			btr_cur_get_page_zip(&cursor),
			offsets, lower_page_no, mtr);
		mem_heap_empty(heap);
	} else {
		lower_page = buf_block_get_frame(block);
		lower_page_no = buf_block_get_page_no(block);
		lower_page_zip = buf_block_get_page_zip(block);
		upper_page = buf_block_get_frame(new_block);
		upper_page_no = buf_block_get_page_no(new_block);
		upper_page_zip = buf_block_get_page_zip(new_block);
	}

	/* Get the level of the split pages */
	level = btr_page_get_level(buf_block_get_frame(block), mtr);

	/* Build the node pointer (= node key and page address) for the upper
	half */

	node_ptr_upper = dict_index_build_node_ptr(index, split_rec,
						   upper_page_no, heap, level);

	/* Insert it next to the pointer to the lower half. Note that this
	may generate recursion leading to a split on the higher level. */

	btr_insert_on_non_leaf_level(index, level + 1, node_ptr_upper, mtr);

	/* Free the memory heap */
	mem_heap_free(heap);

	/* Get the previous and next pages of page */

	prev_page_no = btr_page_get_prev(page, mtr);
	next_page_no = btr_page_get_next(page, mtr);
	space = buf_block_get_space(block);

	/* Update page links of the level */

	if (prev_page_no != FIL_NULL) {
		buf_block_t*	prev_block = btr_block_get(space, prev_page_no,
							   RW_X_LATCH, mtr);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(prev_block->frame) == page_is_comp(page));
		ut_a(btr_page_get_next(prev_block->frame, mtr)
		     == buf_block_get_page_no(block));
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_next(buf_block_get_frame(prev_block),
				  buf_block_get_page_zip(prev_block),
				  lower_page_no, mtr);
	}

	if (next_page_no != FIL_NULL) {
		buf_block_t*	next_block = btr_block_get(space, next_page_no,
							   RW_X_LATCH, mtr);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(next_block->frame) == page_is_comp(page));
		ut_a(btr_page_get_prev(next_block->frame, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_prev(buf_block_get_frame(next_block),
				  buf_block_get_page_zip(next_block),
				  upper_page_no, mtr);
	}

	btr_page_set_prev(lower_page, lower_page_zip, prev_page_no, mtr);
	btr_page_set_next(lower_page, lower_page_zip, upper_page_no, mtr);
	btr_page_set_level(lower_page, lower_page_zip, level, mtr);

	btr_page_set_prev(upper_page, upper_page_zip, lower_page_no, mtr);
	btr_page_set_next(upper_page, upper_page_zip, next_page_no, mtr);
	btr_page_set_level(upper_page, upper_page_zip, level, mtr);
}

/*****************************************************************
Splits an index page to halves and inserts the tuple. It is assumed
that mtr holds an x-latch to the index tree. NOTE: the tree x-latch
is released within this function! NOTE that the operation of this
function must always succeed, we cannot reverse it: therefore
enough free disk space must be guaranteed to be available before
this function is called. */

rec_t*
btr_page_split_and_insert(
/*======================*/
				/* out: inserted record; NOTE: the tree
				x-latch is released! NOTE: 2 free disk
				pages must be available! */
	btr_cur_t*	cursor,	/* in: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	dtuple_t*	tuple,	/* in: tuple to insert */
	const ulint*	ext,	/* in: array of extern field numbers */
	ulint		n_ext,	/* in: number of elements in vec */
	mtr_t*		mtr)	/* in: mtr */
{
	buf_block_t*	block;
	page_t*		page;
	page_zip_des_t*	page_zip;
	ulint		page_no;
	byte		direction;
	ulint		hint_page_no;
	buf_block_t*	new_block;
	page_t*		new_page;
	page_zip_des_t*	new_page_zip;
	rec_t*		split_rec;
	buf_block_t*	left_block;
	buf_block_t*	right_block;
	buf_block_t*	insert_block;
	page_t*		insert_page;
	page_cur_t*	page_cursor;
	rec_t*		first_rec;
	byte*		buf = 0; /* remove warning */
	rec_t*		move_limit;
	ibool		insert_will_fit;
	ibool		insert_left;
	ulint		n_iterations = 0;
	rec_t*		rec;
	mem_heap_t*	heap;
	ulint		n_uniq;
	ulint*		offsets;

	heap = mem_heap_create(1024);
	n_uniq = dict_index_get_n_unique_in_tree(cursor->index);
func_start:
	mem_heap_empty(heap);
	offsets = NULL;

	ut_ad(mtr_memo_contains(mtr, dict_index_get_lock(cursor->index),
				MTR_MEMO_X_LOCK));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(dict_index_get_lock(cursor->index), RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	page_zip = buf_block_get_page_zip(block);

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_get_n_recs(page) >= 1);

	page_no = buf_block_get_page_no(block);

	/* 1. Decide the split record; split_rec == NULL means that the
	tuple to be inserted should be the first record on the upper
	half-page */

	if (n_iterations > 0) {
		direction = FSP_UP;
		hint_page_no = page_no + 1;
		split_rec = btr_page_get_sure_split_rec(cursor, tuple,
							ext, n_ext);

	} else if (btr_page_get_split_rec_to_right(cursor, &split_rec)) {
		direction = FSP_UP;
		hint_page_no = page_no + 1;

	} else if (btr_page_get_split_rec_to_left(cursor, &split_rec)) {
		direction = FSP_DOWN;
		hint_page_no = page_no - 1;
	} else {
		direction = FSP_UP;
		hint_page_no = page_no + 1;
		split_rec = page_get_middle_rec(page);
	}

	/* 2. Allocate a new page to the index */
	new_block = btr_page_alloc(cursor->index, hint_page_no, direction,
				   btr_page_get_level(page, mtr), mtr);
	new_page = buf_block_get_frame(new_block);
	new_page_zip = buf_block_get_page_zip(new_block);
	btr_page_create(new_block, new_page_zip, cursor->index,
			btr_page_get_level(page, mtr), mtr);

	/* 3. Calculate the first record on the upper half-page, and the
	first record (move_limit) on original page which ends up on the
	upper half */

	if (split_rec) {
		first_rec = move_limit = split_rec;
	} else {
		buf = mem_alloc(rec_get_converted_size(cursor->index,
						       tuple, ext, n_ext));

		first_rec = rec_convert_dtuple_to_rec(buf, cursor->index,
						      tuple, ext, n_ext);
		move_limit = page_rec_get_next(btr_cur_get_rec(cursor));
	}

	/* 4. Do first the modifications in the tree structure */

	btr_attach_half_pages(cursor->index, block,
			      first_rec, new_block, direction, mtr);

	/* If the split is made on the leaf level and the insert will fit
	on the appropriate half-page, we may release the tree x-latch.
	We can then move the records after releasing the tree latch,
	thus reducing the tree latch contention. */

	if (split_rec) {
		offsets = rec_get_offsets(split_rec, cursor->index, offsets,
					  n_uniq, &heap);

		insert_left = cmp_dtuple_rec(tuple, split_rec, offsets) < 0;
		insert_will_fit = btr_page_insert_fits(cursor, split_rec,
						       offsets, tuple,
						       ext, n_ext, heap);
	} else {
		mem_free(buf);
		insert_left = FALSE;
		insert_will_fit = btr_page_insert_fits(cursor, NULL,
						       NULL, tuple,
						       ext, n_ext, heap);
	}

	if (insert_will_fit && page_is_leaf(page) && !page_zip) {

		mtr_memo_release(mtr, dict_index_get_lock(cursor->index),
				 MTR_MEMO_X_LOCK);
	}

	/* 5. Move then the records to the new page */
	if (direction == FSP_DOWN) {
		/*		fputs("Split left\n", stderr); */

		if (UNIV_UNLIKELY
		    (!page_move_rec_list_start(new_block, block, move_limit,
					       cursor->index, mtr))) {
			/* For some reason, compressing new_page failed,
			even though it should contain fewer records than
			the original page.  Copy the page byte for byte
			and then delete the records from both pages
			as appropriate.  Deleting will always succeed. */
			ut_a(new_page_zip);

			page_zip_copy(new_page_zip, new_page,
				      page_zip, page, cursor->index, mtr);
			page_delete_rec_list_end(move_limit - page + new_page,
						 new_block, cursor->index,
						 ULINT_UNDEFINED,
						 ULINT_UNDEFINED, mtr);
			page_delete_rec_list_start(move_limit, block,
						   cursor->index, mtr);
		}

		left_block = new_block;
		right_block = block;

		lock_update_split_left(right_block, left_block);
	} else {
		/*		fputs("Split right\n", stderr); */

		if (UNIV_UNLIKELY
		    (!page_move_rec_list_end(new_block, block, move_limit,
					     cursor->index, mtr))) {
			/* For some reason, compressing new_page failed,
			even though it should contain fewer records than
			the original page.  Copy the page byte for byte
			and then delete the records from both pages
			as appropriate.  Deleting will always succeed. */
			ut_a(new_page_zip);

			page_zip_copy(new_page_zip, new_page,
				      page_zip, page, cursor->index, mtr);
			page_delete_rec_list_start(move_limit - page
						   + new_page, new_block,
						   cursor->index, mtr);
			page_delete_rec_list_end(move_limit, block,
						 cursor->index,
						 ULINT_UNDEFINED,
						 ULINT_UNDEFINED, mtr);
		}

		left_block = block;
		right_block = new_block;

		lock_update_split_right(right_block, left_block);
	}

#ifdef UNIV_ZIP_DEBUG
	if (UNIV_LIKELY_NULL(page_zip)) {
		ut_a(page_zip_validate(page_zip, page));
		ut_a(page_zip_validate(new_page_zip, new_page));
	}
#endif /* UNIV_ZIP_DEBUG */

	/* At this point, split_rec, move_limit and first_rec may point
	to garbage on the old page. */

	/* 6. The split and the tree modification is now completed. Decide the
	page where the tuple should be inserted */

	if (insert_left) {
		insert_block = left_block;
	} else {
		insert_block = right_block;
	}

	insert_page = buf_block_get_frame(insert_block);

	/* 7. Reposition the cursor for insert and try insertion */
	page_cursor = btr_cur_get_page_cur(cursor);

	page_cur_search(insert_block, cursor->index, tuple,
			PAGE_CUR_LE, page_cursor);

	rec = page_cur_tuple_insert(page_cursor, tuple,
				    cursor->index, ext, n_ext, mtr);

#ifdef UNIV_ZIP_DEBUG
	{
		page_zip_des_t*	insert_page_zip
			= buf_block_get_page_zip(insert_block);
		ut_a(!insert_page_zip
		     || page_zip_validate(insert_page_zip, insert_page));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (UNIV_LIKELY(rec != NULL)) {
		/* Insert fit on the page: update the free bits for the
		left and right pages in the same mtr */

		ibuf_update_free_bits_for_two_pages_low(cursor->index,
							left_block,
							right_block, mtr);
		/* fprintf(stderr, "Split and insert done %lu %lu\n",
		page_get_page_no(buf_block_get_frame(left_block)),
		page_get_page_no(buf_block_get_frame(right_block))); */
		mem_heap_free(heap);
		return(rec);
	}

	/* 8. If insert did not fit, try page reorganization */

	if (UNIV_UNLIKELY
	    (!btr_page_reorganize(insert_block, cursor->index, mtr))) {

		goto insert_failed;
	}

	page_cur_search(insert_block, cursor->index, tuple,
			PAGE_CUR_LE, page_cursor);
	rec = page_cur_tuple_insert(page_cursor, tuple, cursor->index,
				    ext, n_ext, mtr);

	if (UNIV_UNLIKELY(rec == NULL)) {
		/* The insert did not fit on the page: loop back to the
		start of the function for a new split */
insert_failed:
		/* We play safe and reset the free bits for new_page */
		ibuf_reset_free_bits_with_type(cursor->index->type, new_block);

		/* fprintf(stderr, "Split second round %lu\n",
		page_get_page_no(page)); */
		n_iterations++;
		ut_ad(n_iterations < 2
		      || buf_block_get_page_zip(insert_block));
		ut_ad(!insert_will_fit
		      || buf_block_get_page_zip(insert_block));

		goto func_start;
	}

	/* Insert fit on the page: update the free bits for the
	left and right pages in the same mtr */

	ibuf_update_free_bits_for_two_pages_low(cursor->index, left_block,
						right_block, mtr);
#if 0
	fprintf(stderr, "Split and insert done %lu %lu\n",
		buf_block_get_page_no(left_block),
		buf_block_get_page_no(right_block));
#endif

	ut_ad(page_validate(buf_block_get_frame(left_block), cursor->index));
	ut_ad(page_validate(buf_block_get_frame(right_block), cursor->index));

	mem_heap_free(heap);
	return(rec);
}

/*****************************************************************
Removes a page from the level list of pages. */
static
void
btr_level_list_remove(
/*==================*/
	page_t*		page,	/* in: page to remove */
	mtr_t*		mtr)	/* in: mtr */
{
	ulint	space;
	ulint	prev_page_no;
	ulint	next_page_no;

	ut_ad(page && mtr);
	ut_ad(mtr_memo_contains_page(mtr, page, MTR_MEMO_PAGE_X_FIX));
	/* Get the previous and next page numbers of page */

	prev_page_no = btr_page_get_prev(page, mtr);
	next_page_no = btr_page_get_next(page, mtr);
	space = page_get_space_id(page);

	/* Update page links of the level */

	if (prev_page_no != FIL_NULL) {
		buf_block_t*	prev_block
			= btr_block_get(space, prev_page_no, RW_X_LATCH, mtr);
		page_t*		prev_page
			= buf_block_get_frame(prev_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(prev_page) == page_is_comp(page));
		ut_a(btr_page_get_next(prev_page, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_next(prev_page,
				  buf_block_get_page_zip(prev_block),
				  next_page_no, mtr);
	}

	if (next_page_no != FIL_NULL) {
		buf_block_t*	next_block
			= btr_block_get(space, next_page_no, RW_X_LATCH, mtr);
		page_t*		next_page
			= buf_block_get_frame(next_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(next_page) == page_is_comp(page));
		ut_a(btr_page_get_prev(next_page, mtr)
		     == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

		btr_page_set_prev(next_page,
				  buf_block_get_page_zip(next_block),
				  prev_page_no, mtr);
	}
}

/********************************************************************
Writes the redo log record for setting an index record as the predefined
minimum record. */
UNIV_INLINE
void
btr_set_min_rec_mark_log(
/*=====================*/
	rec_t*	rec,	/* in: record */
	byte	type,	/* in: MLOG_COMP_REC_MIN_MARK or MLOG_REC_MIN_MARK */
	mtr_t*	mtr)	/* in: mtr */
{
	mlog_write_initial_log_record(rec, type, mtr);

	/* Write rec offset as a 2-byte ulint */
	mlog_catenate_ulint(mtr, page_offset(rec), MLOG_2BYTES);
}

/********************************************************************
Parses the redo log record for setting an index record as the predefined
minimum record. */

byte*
btr_parse_set_min_rec_mark(
/*=======================*/
			/* out: end of log record or NULL */
	byte*	ptr,	/* in: buffer */
	byte*	end_ptr,/* in: buffer end */
	ulint	comp,	/* in: nonzero=compact page format */
	page_t*	page,	/* in: page or NULL */
	mtr_t*	mtr)	/* in: mtr or NULL */
{
	rec_t*	rec;

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	if (page) {
		ut_a(!page_is_comp(page) == !comp);

		rec = page + mach_read_from_2(ptr);

		btr_set_min_rec_mark(rec, mtr);
	}

	return(ptr + 2);
}

/********************************************************************
Sets a record as the predefined minimum record. */

void
btr_set_min_rec_mark(
/*=================*/
	rec_t*	rec,	/* in: record */
	mtr_t*	mtr)	/* in: mtr */
{
	ulint	info_bits;

	if (UNIV_LIKELY(page_rec_is_comp(rec))) {
		info_bits = rec_get_info_bits(rec, TRUE);

		rec_set_info_bits_new(rec, info_bits | REC_INFO_MIN_REC_FLAG);

		btr_set_min_rec_mark_log(rec, MLOG_COMP_REC_MIN_MARK, mtr);
	} else {
		info_bits = rec_get_info_bits(rec, FALSE);

		rec_set_info_bits_old(rec, info_bits | REC_INFO_MIN_REC_FLAG);

		btr_set_min_rec_mark_log(rec, MLOG_REC_MIN_MARK, mtr);
	}
}

/*****************************************************************
Deletes on the upper level the node pointer to a page. */

void
btr_node_ptr_delete(
/*================*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: page whose node pointer is deleted */
	mtr_t*		mtr)	/* in: mtr */
{
	btr_cur_t	cursor;
	ibool		compressed;
	ulint		err;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));

	/* Delete node pointer on father page */
	btr_page_get_father(index, block, mtr, &cursor);

	compressed = btr_cur_pessimistic_delete(&err, TRUE, &cursor, FALSE,
						mtr);
	ut_a(err == DB_SUCCESS);

	if (!compressed) {
		btr_cur_compress_if_useful(&cursor, mtr);
	}
}

/*****************************************************************
If page is the only on its level, this function moves its records to the
father page, thus reducing the tree height. */
static
void
btr_lift_page_up(
/*=============*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: page which is the only on its level;
				must not be empty: use
				btr_discard_only_page_on_level if the last
				record from the page should be removed */
	mtr_t*		mtr)	/* in: mtr */
{
	btr_cur_t	father_cursor;
	buf_block_t*	father_block;
	page_t*		father_page;
	ulint		page_level;
	page_zip_des_t*	father_page_zip;
	page_t*		page		= buf_block_get_frame(block);

	ut_ad(btr_page_get_prev(page, mtr) == FIL_NULL);
	ut_ad(btr_page_get_next(page, mtr) == FIL_NULL);
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));

	btr_page_get_father(index, block, mtr, &father_cursor);
	father_block = btr_cur_get_block(&father_cursor);
	father_page_zip = buf_block_get_page_zip(father_block);
	father_page = buf_block_get_frame(father_block);

	page_level = btr_page_get_level(page, mtr);

	btr_search_drop_page_hash_index(block);

	/* Make the father empty */
	btr_page_empty(father_block, father_page_zip, mtr, index);
	/* Set the level before inserting records, because
	page_zip_compress() requires that the first user record
	on a non-leaf page has the min_rec_mark set. */
	btr_page_set_level(father_page, father_page_zip, page_level, mtr);

	/* Copy the records to the father page one by one. */
	if (UNIV_UNLIKELY
	    (!page_copy_rec_list_end(father_block, block,
				     page_get_infimum_rec(page),
				     index, mtr))) {
		ut_a(father_page_zip);

		/* Copy the page byte for byte. */
		page_zip_copy(father_page_zip, father_page,
			      buf_block_get_page_zip(block),
			      page, index, mtr);
	}

	lock_update_copy_and_discard(father_block, block);

	/* Free the file page */
	btr_page_free(index, block, mtr);

	/* We play safe and reset the free bits for the father */
	ibuf_reset_free_bits_with_type(index->type, father_block);
	ut_ad(page_validate(father_page, index));
	ut_ad(btr_check_node_ptr(index, father_block, mtr));
}

/*****************************************************************
Tries to merge the page first to the left immediate brother if such a
brother exists, and the node pointers to the current page and to the brother
reside on the same page. If the left brother does not satisfy these
conditions, looks at the right brother. If the page is the only one on that
level lifts the records of the page to the father page, thus reducing the
tree height. It is assumed that mtr holds an x-latch on the tree and on the
page. If cursor is on the leaf level, mtr must also hold x-latches to the
brothers, if they exist. */

ibool
btr_compress(
/*=========*/
				/* out: TRUE on success */
	btr_cur_t*	cursor,	/* in: cursor on the page to merge or lift;
				the page must not be empty: in record delete
				use btr_discard_page if the page would become
				empty */
	mtr_t*		mtr)	/* in: mtr */
{
	dict_index_t*	index;
	ulint		space;
	ulint		left_page_no;
	ulint		right_page_no;
	buf_block_t*	merge_block;
	page_t*		merge_page;
	page_zip_des_t*	merge_page_zip;
	ibool		is_left;
	buf_block_t*	block;
	page_t*		page;
	btr_cur_t	father_cursor;
	mem_heap_t*	heap;
	ulint*		offsets;
	ulint		data_size;
	ulint		n_recs;
	ulint		max_ins_size;
	ulint		max_ins_size_reorg;
	ulint		level;

	block = btr_cur_get_block(cursor);
	page = btr_cur_get_page(cursor);
	index = btr_cur_get_index(cursor);
	ut_a((ibool) !!page_is_comp(page) == dict_table_is_comp(index->table));

	ut_ad(mtr_memo_contains(mtr, dict_index_get_lock(index),
				MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	level = btr_page_get_level(page, mtr);
	space = dict_index_get_space(index);

	left_page_no = btr_page_get_prev(page, mtr);
	right_page_no = btr_page_get_next(page, mtr);

#if 0
	fprintf(stderr, "Merge left page %lu right %lu \n",
		left_page_no, right_page_no);
#endif

	heap = mem_heap_create(100);
	offsets = btr_page_get_father_block(NULL, heap, index, block, mtr,
					    &father_cursor);

	/* Decide the page to which we try to merge and which will inherit
	the locks */

	is_left = left_page_no != FIL_NULL;

	if (is_left) {

		merge_block = btr_block_get(space, left_page_no, RW_X_LATCH,
					    mtr);
		merge_page = buf_block_get_frame(merge_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(btr_page_get_next(merge_page, mtr) == block->offset);
#endif /* UNIV_BTR_DEBUG */
	} else if (right_page_no != FIL_NULL) {

		merge_block = btr_block_get(space, right_page_no, RW_X_LATCH,
					    mtr);
		merge_page = buf_block_get_frame(merge_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(btr_page_get_prev(merge_page, mtr) == block->offset);
#endif /* UNIV_BTR_DEBUG */
	} else {
		/* The page is the only one on the level, lift the records
		to the father */
		btr_lift_page_up(index, block, mtr);
		mem_heap_free(heap);
		return(TRUE);
	}

	n_recs = page_get_n_recs(page);
	data_size = page_get_data_size(page);
#ifdef UNIV_BTR_DEBUG
	ut_a(page_is_comp(merge_page) == page_is_comp(page));
#endif /* UNIV_BTR_DEBUG */

	max_ins_size_reorg = page_get_max_insert_size_after_reorganize(
		merge_page, n_recs);
	if (data_size > max_ins_size_reorg) {

		/* No space for merge */
err_exit:
		mem_heap_free(heap);
		return(FALSE);
	}

	ut_ad(page_validate(merge_page, index));

	max_ins_size = page_get_max_insert_size(merge_page, n_recs);

	if (UNIV_UNLIKELY(data_size > max_ins_size)) {

		/* We have to reorganize merge_page */

		if (UNIV_UNLIKELY(!btr_page_reorganize(merge_block,
						       index, mtr))) {

			goto err_exit;
		}

		max_ins_size = page_get_max_insert_size(merge_page, n_recs);

		ut_ad(page_validate(merge_page, index));
		ut_ad(max_ins_size == max_ins_size_reorg);

		if (UNIV_UNLIKELY(data_size > max_ins_size)) {

			/* Add fault tolerance, though this should
			never happen */

			goto err_exit;
		}
	}

	merge_page_zip = buf_block_get_page_zip(merge_block);
#ifdef UNIV_ZIP_DEBUG
	if (UNIV_LIKELY_NULL(merge_page_zip)) {
		ut_a(page_zip_validate(merge_page_zip, merge_page));
		ut_a(page_zip_validate(buf_block_get_page_zip(block), page));
	}
#endif /* UNIV_ZIP_DEBUG */

	/* Move records to the merge page */
	if (is_left) {
		rec_t*	orig_pred = page_copy_rec_list_start(
			merge_block, block, page_get_supremum_rec(page),
			index, mtr);

		if (UNIV_UNLIKELY(!orig_pred)) {
			goto err_exit;
		}

		btr_search_drop_page_hash_index(block);

		/* Remove the page from the level list */
		btr_level_list_remove(page, mtr);

		btr_node_ptr_delete(index, block, mtr);
		lock_update_merge_left(merge_block, orig_pred, block);
	} else {
		rec_t*		orig_succ;
#ifdef UNIV_BTR_DEBUG
		byte		fil_page_prev[4];
#endif /* UNIV_BTR_DEBUG */

		if (UNIV_LIKELY_NULL(merge_page_zip)) {
			/* The function page_zip_compress(), which will be
			invoked by page_copy_rec_list_end() below,
			requires that FIL_PAGE_PREV be FIL_NULL.
			Clear the field, but prepare to restore it. */
#ifdef UNIV_BTR_DEBUG
			memcpy(fil_page_prev, merge_page + FIL_PAGE_PREV, 4);
#endif /* UNIV_BTR_DEBUG */
#if FIL_NULL != 0xffffffff
# error "FIL_NULL != 0xffffffff"
#endif
			memset(merge_page + FIL_PAGE_PREV, 0xff, 4);
		}

		orig_succ = page_copy_rec_list_end(merge_block, block,
						   page_get_infimum_rec(page),
						   cursor->index, mtr);

		if (UNIV_UNLIKELY(!orig_succ)) {
			ut_a(merge_page_zip);
			/* FIL_PAGE_PREV was restored from merge_page_zip. */
			ut_ad(!memcmp(fil_page_prev,
				      merge_page + FIL_PAGE_PREV, 4));
			goto err_exit;
		}

		btr_search_drop_page_hash_index(block);

#ifdef UNIV_BTR_DEBUG
		if (UNIV_LIKELY_NULL(merge_page_zip)) {
			/* Restore FIL_PAGE_PREV in order to avoid an assertion
			failure in btr_level_list_remove(), which will set
			the field again to FIL_NULL.  Even though this makes
			merge_page and merge_page_zip inconsistent for a
			split second, it is harmless, because the pages
			are X-latched. */
			memcpy(merge_page + FIL_PAGE_PREV, fil_page_prev, 4);
		}
#endif /* UNIV_BTR_DEBUG */

		/* Remove the page from the level list */
		btr_level_list_remove(page, mtr);

		/* Replace the address of the old child node (= page) with the
		address of the merge page to the right */

		btr_node_ptr_set_child_page_no(
			btr_cur_get_rec(&father_cursor),
			btr_cur_get_page_zip(&father_cursor),
			offsets, right_page_no, mtr);
		btr_node_ptr_delete(index, merge_block, mtr);

		lock_update_merge_right(merge_block, orig_succ, block);
	}

	mem_heap_free(heap);

	/* We have added new records to merge_page: update its free bits */
	ibuf_update_free_bits_if_full(index, merge_block,
				      UNIV_PAGE_SIZE, ULINT_UNDEFINED);

	ut_ad(page_validate(merge_page, index));

	/* Free the file page */
	btr_page_free(index, block, mtr);

	ut_ad(btr_check_node_ptr(index, merge_block, mtr));
	return(TRUE);
}

/*****************************************************************
Discards a page that is the only page on its level. */
static
void
btr_discard_only_page_on_level(
/*===========================*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: page which is the only on its level */
	mtr_t*		mtr)	/* in: mtr */
{
	btr_cur_t	father_cursor;
	buf_block_t*	father_block;
	page_t*		father_page;
	page_zip_des_t*	father_page_zip;
	page_t*		page		= buf_block_get_frame(block);
	ulint		page_level;

	ut_ad(btr_page_get_prev(page, mtr) == FIL_NULL);
	ut_ad(btr_page_get_next(page, mtr) == FIL_NULL);
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	btr_search_drop_page_hash_index(block);

	btr_page_get_father(index, block, mtr, &father_cursor);
	father_block = btr_cur_get_block(&father_cursor);
	father_page_zip = buf_block_get_page_zip(father_block);
	father_page = buf_block_get_frame(father_block);

	page_level = btr_page_get_level(page, mtr);

	lock_update_discard(father_block, PAGE_HEAP_NO_SUPREMUM, block);

	btr_page_set_level(father_page, father_page_zip, page_level, mtr);

	/* Free the file page */
	btr_page_free(index, block, mtr);

	if (UNIV_LIKELY(buf_block_get_page_no(father_block)
			== dict_index_get_page(index))) {
		/* The father is the root page */

		btr_page_empty(father_block, father_page_zip, mtr, index);

		/* We play safe and reset the free bits for the father */
		ibuf_reset_free_bits_with_type(index->type, father_block);
	} else {
		ut_ad(page_get_n_recs(father_page) == 1);

		btr_discard_only_page_on_level(index, father_block, mtr);
	}
}

/*****************************************************************
Discards a page from a B-tree. This is used to remove the last record from
a B-tree page: the whole page must be removed at the same time. This cannot
be used for the root page, which is allowed to be empty. */

void
btr_discard_page(
/*=============*/
	btr_cur_t*	cursor,	/* in: cursor on the page to discard: not on
				the root page */
	mtr_t*		mtr)	/* in: mtr */
{
	dict_index_t*	index;
	ulint		space;
	ulint		left_page_no;
	ulint		right_page_no;
	buf_block_t*	merge_block;
	page_t*		merge_page;
	buf_block_t*	block;
	page_t*		page;
	rec_t*		node_ptr;

	block = btr_cur_get_block(cursor);
	index = btr_cur_get_index(cursor);

	ut_ad(dict_index_get_page(index) != block->offset);
	ut_ad(mtr_memo_contains(mtr, dict_index_get_lock(index),
				MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	space = dict_index_get_space(index);

	/* Decide the page which will inherit the locks */

	left_page_no = btr_page_get_prev(buf_block_get_frame(block), mtr);
	right_page_no = btr_page_get_next(buf_block_get_frame(block), mtr);

	if (left_page_no != FIL_NULL) {
		merge_block = btr_block_get(space, left_page_no, RW_X_LATCH,
					    mtr);
		merge_page = buf_block_get_frame(merge_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(btr_page_get_next(merge_page, mtr) == block->offset);
#endif /* UNIV_BTR_DEBUG */
	} else if (right_page_no != FIL_NULL) {
		merge_block = btr_block_get(space, right_page_no, RW_X_LATCH,
					    mtr);
		merge_page = buf_block_get_frame(merge_block);
#ifdef UNIV_BTR_DEBUG
		ut_a(btr_page_get_prev(merge_page, mtr) == block->offset);
#endif /* UNIV_BTR_DEBUG */
	} else {
		btr_discard_only_page_on_level(index, block, mtr);

		return;
	}

	page = buf_block_get_frame(block);
	ut_a(page_is_comp(merge_page) == page_is_comp(page));
	btr_search_drop_page_hash_index(block);

	if (left_page_no == FIL_NULL && !page_is_leaf(page)) {

		/* We have to mark the leftmost node pointer on the right
		side page as the predefined minimum record */
		node_ptr = page_rec_get_next(page_get_infimum_rec(merge_page));

		ut_ad(page_rec_is_user_rec(node_ptr));

		/* This will make page_zip_validate() fail on merge_page
		until btr_level_list_remove() completes.  This is harmless,
		because everything will take place within a single
		mini-transaction and because writing to the redo log
		is an atomic operation (performed by mtr_commit()). */
		btr_set_min_rec_mark(node_ptr, mtr);
	}

	btr_node_ptr_delete(index, block, mtr);

	/* Remove the page from the level list */
	btr_level_list_remove(page, mtr);
#ifdef UNIV_ZIP_DEBUG
	{
		page_zip_des_t*	merge_page_zip
			= buf_block_get_page_zip(merge_block);
		ut_a(!merge_page_zip
		     || page_zip_validate(merge_page_zip, merge_page));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (left_page_no != FIL_NULL) {
		lock_update_discard(merge_block, PAGE_HEAP_NO_SUPREMUM,
				    block);
	} else {
		lock_update_discard(merge_block,
				    lock_get_min_heap_no(merge_block),
				    block);
	}

	/* Free the file page */
	btr_page_free(index, block, mtr);

	ut_ad(btr_check_node_ptr(index, merge_block, mtr));
}

#ifdef UNIV_BTR_PRINT
/*****************************************************************
Prints size info of a B-tree. */

void
btr_print_size(
/*===========*/
	dict_index_t*	index)	/* in: index tree */
{
	page_t*		root;
	fseg_header_t*	seg;
	mtr_t		mtr;

	if (index->type & DICT_IBUF) {
		fputs("Sorry, cannot print info of an ibuf tree:"
		      " use ibuf functions\n", stderr);

		return;
	}

	mtr_start(&mtr);

	root = btr_root_get(index, &mtr);

	seg = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	fputs("INFO OF THE NON-LEAF PAGE SEGMENT\n", stderr);
	fseg_print(seg, &mtr);

	if (!(index->type & DICT_UNIVERSAL)) {

		seg = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;

		fputs("INFO OF THE LEAF PAGE SEGMENT\n", stderr);
		fseg_print(seg, &mtr);
	}

	mtr_commit(&mtr);
}

/****************************************************************
Prints recursively index tree pages. */
static
void
btr_print_recursive(
/*================*/
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: index page */
	ulint		width,	/* in: print this many entries from start
				and end */
	mem_heap_t**	heap,	/* in/out: heap for rec_get_offsets() */
	ulint**		offsets,/* in/out: buffer for rec_get_offsets() */
	mtr_t*		mtr)	/* in: mtr */
{
	const page_t*	page	= buf_block_get_frame(block);
	page_cur_t	cursor;
	ulint		n_recs;
	ulint		i	= 0;
	mtr_t		mtr2;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	fprintf(stderr, "NODE ON LEVEL %lu page number %lu\n",
		(ulong) btr_page_get_level(page, mtr),
		(ulong) buf_block_get_page_no(block));

	page_print(block, index, width, width);

	n_recs = page_get_n_recs(page);

	page_cur_set_before_first(block, &cursor);
	page_cur_move_to_next(&cursor);

	while (!page_cur_is_after_last(&cursor)) {

		if (page_is_leaf(page)) {

			/* If this is the leaf level, do nothing */

		} else if ((i <= width) || (i >= n_recs - width)) {

			const rec_t*	node_ptr;

			mtr_start(&mtr2);

			node_ptr = page_cur_get_rec(&cursor);

			*offsets = rec_get_offsets(node_ptr, index, *offsets,
						   ULINT_UNDEFINED, heap);
			btr_print_recursive(index,
					    btr_node_ptr_get_child(node_ptr,
								   *offsets,
								   &mtr2),
					    width, heap, offsets, &mtr2);
			mtr_commit(&mtr2);
		}

		page_cur_move_to_next(&cursor);
		i++;
	}
}

/******************************************************************
Prints directories and other info of all nodes in the tree. */

void
btr_print_index(
/*============*/
	dict_index_t*	index,	/* in: index */
	ulint		width)	/* in: print this many entries from start
				and end */
{
	mtr_t		mtr;
	buf_block_t*	root;
	mem_heap_t*	heap	= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets	= offsets_;
	*offsets_ = (sizeof offsets_) / sizeof *offsets_;

	fputs("--------------------------\n"
	      "INDEX TREE PRINT\n", stderr);

	mtr_start(&mtr);

	root = btr_root_block_get(index, &mtr);

	btr_print_recursive(index, root, width, &heap, &offsets, &mtr);
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	mtr_commit(&mtr);

	btr_validate_index(index, NULL);
}
#endif /* UNIV_BTR_PRINT */

#ifdef UNIV_DEBUG
/****************************************************************
Checks that the node pointer to a page is appropriate. */

ibool
btr_check_node_ptr(
/*===============*/
				/* out: TRUE */
	dict_index_t*	index,	/* in: index tree */
	buf_block_t*	block,	/* in: index page */
	mtr_t*		mtr)	/* in: mtr */
{
	mem_heap_t*	heap;
	dtuple_t*	tuple;
	ulint*		offsets;
	btr_cur_t	cursor;
	page_t*		page = buf_block_get_frame(block);

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	if (dict_index_get_page(index) == block->offset) {

		return(TRUE);
	}

	heap = mem_heap_create(256);
	offsets = btr_page_get_father_block(NULL, heap, index, block, mtr,
					    &cursor);

	if (page_is_leaf(page)) {

		goto func_exit;
	}

	tuple = dict_index_build_node_ptr(
		index, page_rec_get_next(page_get_infimum_rec(page)), 0, heap,
		btr_page_get_level(page, mtr));

	ut_a(!cmp_dtuple_rec(tuple, btr_cur_get_rec(&cursor), offsets));
func_exit:
	mem_heap_free(heap);

	return(TRUE);
}
#endif /* UNIV_DEBUG */

/****************************************************************
Display identification information for a record. */
static
void
btr_index_rec_validate_report(
/*==========================*/
	page_t*		page,	/* in: index page */
	rec_t*		rec,	/* in: index record */
	dict_index_t*	index)	/* in: index */
{
	fputs("InnoDB: Record in ", stderr);
	dict_index_name_print(stderr, NULL, index);
	fprintf(stderr, ", page %lu, at offset %lu\n",
		page_get_page_no(page), (ulint) page_offset(rec));
}

/****************************************************************
Checks the size and number of fields in a record based on the definition of
the index. */

ibool
btr_index_rec_validate(
/*===================*/
					/* out: TRUE if ok */
	rec_t*		rec,		/* in: index record */
	dict_index_t*	index,		/* in: index */
	ibool		dump_on_error)	/* in: TRUE if the function
					should print hex dump of record
					and page on error */
{
	ulint		len;
	ulint		n;
	ulint		i;
	page_t*		page;
	mem_heap_t*	heap	= NULL;
	ulint		offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		offsets	= offsets_;
	*offsets_ = (sizeof offsets_) / sizeof *offsets_;

	page = page_align(rec);

	if (UNIV_UNLIKELY(index->type & DICT_UNIVERSAL)) {
		/* The insert buffer index tree can contain records from any
		other index: we cannot check the number of fields or
		their length */

		return(TRUE);
	}

	if (UNIV_UNLIKELY((ibool)!!page_is_comp(page)
			  != dict_table_is_comp(index->table))) {
		btr_index_rec_validate_report(page, rec, index);
		fprintf(stderr, "InnoDB: compact flag=%lu, should be %lu\n",
			(ulong) !!page_is_comp(page),
			(ulong) dict_table_is_comp(index->table));

		return(FALSE);
	}

	n = dict_index_get_n_fields(index);

	if (!page_is_comp(page)
	    && UNIV_UNLIKELY(rec_get_n_fields_old(rec) != n)) {
		btr_index_rec_validate_report(page, rec, index);
		fprintf(stderr, "InnoDB: has %lu fields, should have %lu\n",
			(ulong) rec_get_n_fields_old(rec), (ulong) n);

		if (dump_on_error) {
			buf_page_print(page, 0);

			fputs("InnoDB: corrupt record ", stderr);
			rec_print_old(stderr, rec);
			putc('\n', stderr);
		}
		return(FALSE);
	}

	offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED, &heap);

	for (i = 0; i < n; i++) {
		ulint	fixed_size = dict_col_get_fixed_size(
			dict_index_get_nth_col(index, i));

		rec_get_nth_field_offs(offsets, i, &len);

		/* Note that prefix indexes are not fixed size even when
		their type is CHAR. */

		if ((dict_index_get_nth_field(index, i)->prefix_len == 0
		     && len != UNIV_SQL_NULL && fixed_size
		     && len != fixed_size)
		    || (dict_index_get_nth_field(index, i)->prefix_len > 0
			&& len != UNIV_SQL_NULL
			&& len
			> dict_index_get_nth_field(index, i)->prefix_len)) {

			btr_index_rec_validate_report(page, rec, index);
			fprintf(stderr,
				"InnoDB: field %lu len is %lu,"
				" should be %lu\n",
				(ulong) i, (ulong) len, (ulong) fixed_size);

			if (dump_on_error) {
				buf_page_print(page, 0);

				fputs("InnoDB: corrupt record ", stderr);
				rec_print_new(stderr, rec, offsets);
				putc('\n', stderr);
			}
			if (UNIV_LIKELY_NULL(heap)) {
				mem_heap_free(heap);
			}
			return(FALSE);
		}
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(TRUE);
}

/****************************************************************
Checks the size and number of fields in records based on the definition of
the index. */
static
ibool
btr_index_page_validate(
/*====================*/
				/* out: TRUE if ok */
	buf_block_t*	block,	/* in: index page */
	dict_index_t*	index)	/* in: index */
{
	page_cur_t	cur;
	ibool		ret	= TRUE;

	page_cur_set_before_first(block, &cur);
	page_cur_move_to_next(&cur);

	for (;;) {
		if (page_cur_is_after_last(&cur)) {

			break;
		}

		if (!btr_index_rec_validate(cur.rec, index, TRUE)) {

			return(FALSE);
		}

		page_cur_move_to_next(&cur);
	}

	return(ret);
}

/****************************************************************
Report an error on one page of an index tree. */
static
void
btr_validate_report1(
/*=================*/
					/* out: TRUE if ok */
	dict_index_t*		index,	/* in: index */
	ulint			level,	/* in: B-tree level */
	const buf_block_t*	block)	/* in: index page */
{
	fprintf(stderr, "InnoDB: Error in page %lu of ",
		buf_block_get_page_no(block));
	dict_index_name_print(stderr, NULL, index);
	if (level) {
		fprintf(stderr, ", index tree level %lu", level);
	}
	putc('\n', stderr);
}

/****************************************************************
Report an error on two pages of an index tree. */
static
void
btr_validate_report2(
/*=================*/
					/* out: TRUE if ok */
	const dict_index_t*	index,	/* in: index */
	ulint			level,	/* in: B-tree level */
	const buf_block_t*	block1,	/* in: first index page */
	const buf_block_t*	block2)	/* in: second index page */
{
	fprintf(stderr, "InnoDB: Error in pages %lu and %lu of ",
		buf_block_get_page_no(block1),
		buf_block_get_page_no(block2));
	dict_index_name_print(stderr, NULL, index);
	if (level) {
		fprintf(stderr, ", index tree level %lu", level);
	}
	putc('\n', stderr);
}

/****************************************************************
Validates index tree level. */
static
ibool
btr_validate_level(
/*===============*/
				/* out: TRUE if ok */
	dict_index_t*	index,	/* in: index tree */
	trx_t*		trx,	/* in: transaction or NULL */
	ulint		level)	/* in: level number */
{
	ulint		space;
	buf_block_t*	block;
	page_t*		page;
	buf_block_t*	right_block = 0; /* remove warning */
	page_t*		right_page = 0; /* remove warning */
	page_t*		father_page;
	btr_cur_t	node_cur;
	btr_cur_t	right_node_cur;
	rec_t*		rec;
	ulint		right_page_no;
	ulint		left_page_no;
	page_cur_t	cursor;
	dtuple_t*	node_ptr_tuple;
	ibool		ret	= TRUE;
	mtr_t		mtr;
	mem_heap_t*	heap	= mem_heap_create(256);
	ulint*		offsets	= NULL;
	ulint*		offsets2= NULL;
#ifdef UNIV_ZIP_DEBUG
	page_zip_des_t*	page_zip;
#endif /* UNIV_ZIP_DEBUG */

	mtr_start(&mtr);

	mtr_x_lock(dict_index_get_lock(index), &mtr);

	block = btr_root_block_get(index, &mtr);
	page = buf_block_get_frame(block);

	space = page_get_space_id(page);

	while (level != btr_page_get_level(page, &mtr)) {
		const rec_t*	node_ptr;

#ifdef UNIV_ZIP_DEBUG
		page_zip = buf_block_get_page_zip(block);
		ut_a(!page_zip || page_zip_validate(page_zip, page));
#endif /* UNIV_ZIP_DEBUG */
		ut_a(!page_is_leaf(page));

		page_cur_set_before_first(block, &cursor);
		page_cur_move_to_next(&cursor);

		node_ptr = page_cur_get_rec(&cursor);
		offsets = rec_get_offsets(node_ptr, index, offsets,
					  ULINT_UNDEFINED, &heap);
		block = btr_node_ptr_get_child(node_ptr, offsets, &mtr);
		page = buf_block_get_frame(block);
	}

	/* Now we are on the desired level. Loop through the pages on that
	level. */
loop:
	if (trx_is_interrupted(trx)) {
		mtr_commit(&mtr);
		mem_heap_free(heap);
		return(ret);
	}
	mem_heap_empty(heap);
	offsets = offsets2 = NULL;
	mtr_x_lock(dict_index_get_lock(index), &mtr);

#ifdef UNIV_ZIP_DEBUG
	page_zip = buf_block_get_page_zip(block);
	ut_a(!page_zip || page_zip_validate(page_zip, page));
#endif /* UNIV_ZIP_DEBUG */

	/* Check ordering etc. of records */

	if (!page_validate(page, index)) {
		btr_validate_report1(index, level, block);

		ret = FALSE;
	} else if (level == 0) {
		/* We are on level 0. Check that the records have the right
		number of fields, and field lengths are right. */

		if (!btr_index_page_validate(block, index)) {

			ret = FALSE;
		}
	}

	ut_a(btr_page_get_level(page, &mtr) == level);

	right_page_no = btr_page_get_next(page, &mtr);
	left_page_no = btr_page_get_prev(page, &mtr);

	ut_a(page_get_n_recs(page) > 0 || (level == 0
					   && page_get_page_no(page)
					   == dict_index_get_page(index)));

	if (right_page_no != FIL_NULL) {
		const rec_t*	right_rec;
		right_block = btr_block_get(space, right_page_no, RW_X_LATCH,
					    &mtr);
		right_page = buf_block_get_frame(right_block);
		if (UNIV_UNLIKELY(btr_page_get_prev(right_page, &mtr)
				  != page_get_page_no(page))) {
			btr_validate_report2(index, level, block, right_block);
			fputs("InnoDB: broken FIL_PAGE_NEXT"
			      " or FIL_PAGE_PREV links\n", stderr);
			buf_page_print(page, 0);
			buf_page_print(right_page, 0);

			ret = FALSE;
		}

		if (UNIV_UNLIKELY(page_is_comp(right_page)
				  != page_is_comp(page))) {
			btr_validate_report2(index, level, block, right_block);
			fputs("InnoDB: 'compact' flag mismatch\n", stderr);
			buf_page_print(page, 0);
			buf_page_print(right_page, 0);

			ret = FALSE;

			goto node_ptr_fails;
		}

		rec = page_rec_get_prev(page_get_supremum_rec(page));
		right_rec = page_rec_get_next(page_get_infimum_rec(
						      right_page));
		offsets = rec_get_offsets(rec, index,
					  offsets, ULINT_UNDEFINED, &heap);
		offsets2 = rec_get_offsets(right_rec, index,
					   offsets2, ULINT_UNDEFINED, &heap);
		if (UNIV_UNLIKELY(cmp_rec_rec(rec, right_rec,
					      offsets, offsets2,
					      index) >= 0)) {

			btr_validate_report2(index, level, block, right_block);

			fputs("InnoDB: records in wrong order"
			      " on adjacent pages\n", stderr);

			buf_page_print(page, 0);
			buf_page_print(right_page, 0);

			fputs("InnoDB: record ", stderr);
			rec = page_rec_get_prev(page_get_supremum_rec(page));
			rec_print(stderr, rec, index);
			putc('\n', stderr);
			fputs("InnoDB: record ", stderr);
			rec = page_rec_get_next(
				page_get_infimum_rec(right_page));
			rec_print(stderr, rec, index);
			putc('\n', stderr);

			ret = FALSE;
		}
	}

	if (level > 0 && left_page_no == FIL_NULL) {
		ut_a(REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
			     page_rec_get_next(page_get_infimum_rec(page)),
			     page_is_comp(page)));
	}

	if (buf_block_get_page_no(block) != dict_index_get_page(index)) {

		/* Check father node pointers */

		const rec_t*	node_ptr;

		offsets = btr_page_get_father_block(offsets, heap, index,
						    block, &mtr, &node_cur);
		father_page = btr_cur_get_page(&node_cur);
		node_ptr = btr_cur_get_rec(&node_cur);

		btr_cur_position(
			index, page_rec_get_prev(page_get_supremum_rec(page)),
			block, &node_cur);
		offsets = btr_page_get_father_node_ptr(offsets, heap,
						       &node_cur, &mtr);

		if (UNIV_UNLIKELY(node_ptr != btr_cur_get_rec(&node_cur))
		    || UNIV_UNLIKELY(btr_node_ptr_get_child_page_no(node_ptr,
								    offsets)
				     != buf_block_get_page_no(block))) {

			btr_validate_report1(index, level, block);

			fputs("InnoDB: node pointer to the page is wrong\n",
			      stderr);

			buf_page_print(father_page, 0);
			buf_page_print(page, 0);

			fputs("InnoDB: node ptr ", stderr);
			rec_print(stderr, node_ptr, index);

			rec = btr_cur_get_rec(&node_cur);
			fprintf(stderr, "\n"
				"InnoDB: node ptr child page n:o %lu\n",
				(ulong) btr_node_ptr_get_child_page_no(
					rec, offsets));

			fputs("InnoDB: record on page ", stderr);
			rec_print_new(stderr, rec, offsets);
			putc('\n', stderr);
			ret = FALSE;

			goto node_ptr_fails;
		}

		if (!page_is_leaf(page)) {
			node_ptr_tuple = dict_index_build_node_ptr(
				index,
				page_rec_get_next(page_get_infimum_rec(page)),
				0, heap, btr_page_get_level(page, &mtr));

			if (cmp_dtuple_rec(node_ptr_tuple, node_ptr,
					   offsets)) {
				const rec_t* first_rec = page_rec_get_next(
					page_get_infimum_rec(page));

				btr_validate_report1(index, level, block);

				buf_page_print(father_page, 0);
				buf_page_print(page, 0);

				fputs("InnoDB: Error: node ptrs differ"
				      " on levels > 0\n"
				      "InnoDB: node ptr ", stderr);
				rec_print_new(stderr, node_ptr, offsets);
				fputs("InnoDB: first rec ", stderr);
				rec_print(stderr, first_rec, index);
				putc('\n', stderr);
				ret = FALSE;

				goto node_ptr_fails;
			}
		}

		if (left_page_no == FIL_NULL) {
			ut_a(node_ptr == page_rec_get_next(
				     page_get_infimum_rec(father_page)));
			ut_a(btr_page_get_prev(father_page, &mtr) == FIL_NULL);
		}

		if (right_page_no == FIL_NULL) {
			ut_a(node_ptr == page_rec_get_prev(
				     page_get_supremum_rec(father_page)));
			ut_a(btr_page_get_next(father_page, &mtr) == FIL_NULL);
		} else {
			offsets = btr_page_get_father_block(
				offsets, heap, index, right_block,
				&mtr, &right_node_cur);
			if (page_rec_get_next((rec_t*) node_ptr)
			    != page_get_supremum_rec(father_page)) {

				if (btr_cur_get_rec(&right_node_cur)
				    != page_rec_get_next((rec_t*) node_ptr)) {
					ret = FALSE;
					fputs("InnoDB: node pointer to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);

					buf_page_print(father_page, 0);
					buf_page_print(page, 0);
					buf_page_print(right_page, 0);
				}
			} else {
				page_t*	right_father_page
					= btr_cur_get_page(&right_node_cur);

				if (btr_cur_get_rec(&right_node_cur)
				    != page_rec_get_next(
					    page_get_infimum_rec(
						    right_father_page))) {
					ret = FALSE;
					fputs("InnoDB: node pointer 2 to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);

					buf_page_print(father_page, 0);
					buf_page_print(right_father_page, 0);
					buf_page_print(page, 0);
					buf_page_print(right_page, 0);
				}

				if (page_get_page_no(right_father_page)
				    != btr_page_get_next(father_page, &mtr)) {

					ret = FALSE;
					fputs("InnoDB: node pointer 3 to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);

					buf_page_print(father_page, 0);
					buf_page_print(right_father_page, 0);
					buf_page_print(page, 0);
					buf_page_print(right_page, 0);
				}
			}
		}
	}

node_ptr_fails:
	/* Commit the mini-transaction to release the latch on 'page'.
	Re-acquire the latch on right_page, which will become 'page'
	on the next loop.  The page has already been checked. */
	mtr_commit(&mtr);

	if (right_page_no != FIL_NULL) {
		mtr_start(&mtr);

		block = btr_block_get(space, right_page_no, RW_X_LATCH, &mtr);
		page = buf_block_get_frame(block);

		goto loop;
	}

	mem_heap_free(heap);
	return(ret);
}

/******************************************************************
Checks the consistency of an index tree. */

ibool
btr_validate_index(
/*===============*/
				/* out: TRUE if ok */
	dict_index_t*	index,	/* in: index */
	trx_t*		trx)	/* in: transaction or NULL */
{
	mtr_t	mtr;
	page_t*	root;
	ulint	i;
	ulint	n;

	mtr_start(&mtr);
	mtr_x_lock(dict_index_get_lock(index), &mtr);

	root = btr_root_get(index, &mtr);
	n = btr_page_get_level(root, &mtr);

	for (i = 0; i <= n && !trx_is_interrupted(trx); i++) {
		if (!btr_validate_level(index, trx, n - i)) {

			mtr_commit(&mtr);

			return(FALSE);
		}
	}

	mtr_commit(&mtr);

	return(TRUE);
}

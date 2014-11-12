/*
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_FIND_ROOT_H
#define __BTRFS_FIND_ROOT_H
#include "ctree.h"
#include "list.h"
#include "kerncompat.h"
/*
 * find root result is a list like the following:
 *
 * <list_head>
 * result_list
 *    |
 *    <gen_entry>	<gen_entry>		<gen_entry>
 *    gen_list	<->	gen_list	<->	gen_list ...
 *    gen:4		gen:5			gen:6
 *    level:0		level:1			level:2
 *    level_list	level_list		level_list
 *    |<eb_entry>	|<eb_entry>		|<eb_entry>
 *    |-l 0's eb	|-l 1'eb(possible root)	|-l 2'eb(possible root)
 *    |-l 0's eb
 *    ...
 *    level_list only contains the highest level's eb.
 *    if level_list only contains 1 eb, that may be root.
 *    if multiple, the root is already overwritten.
 */
struct find_root_eb_entry {
	struct list_head list;
	struct extent_buffer *eb;
};

struct find_root_gen_entry {
	struct list_head gen_list;
	struct list_head eb_list;
	u64 generation;
	u8 level;
};

struct find_root_search_filter {
	u64 objectid;
	u64 generation;
	u64 level;
	u64 super_gen;
	int search_all;
};
int find_root_start(struct btrfs_root *chunk_root,
		    struct list_head *result_list,
		    struct find_root_search_filter *search);
void find_root_free(struct list_head *result_list);
#endif

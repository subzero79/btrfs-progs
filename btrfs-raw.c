/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "utils.h"
#include "disk-io.h"

static int print_usage(void)
{
	fprintf(stderr, "usage: btrfs-raw [ -r block|-w block] device\n");
	exit(1);
}

static int read_block(struct btrfs_root *root, u64 block_nr,
		      struct extent_buffer **eb)
{
	struct extent_buffer *leaf;
	leaf = read_tree_block(root,
			       block_nr,
			       root->leafsize, 0);
	
	if (leaf && btrfs_header_level(leaf) != 0) {
		free_extent_buffer(leaf);
		leaf = NULL;
	}
	
	if (!leaf) {
		leaf = read_tree_block(root,
				       block_nr,
				       root->nodesize, 0);
	}
	if (!leaf) {
		fprintf(stderr, "failed to read %llu\n",
			(unsigned long long)block_nr);
		return -1;
	}

	*eb = leaf;
	return btrfs_is_leaf(leaf) ? root->leafsize : root->nodesize;
}

int main(int ac, char **av)
{
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	struct extent_buffer *eb = NULL;
	struct btrfs_trans_handle *trans = NULL;
	u64 block = ~0ULL;
	int len;
	enum btrfs_open_ctree_flags flags = OPEN_CTREE_PARTIAL;
	radix_tree_init();

	while(1) {
		int c;
		c = getopt(ac, av, "r:w:");
		if (c < 0)
			break;
		switch(c) {
			case 'r':
				block = arg_strtou64(optarg);
				break;
			case 'w':
				flags |= OPEN_CTREE_WRITES;
				block = arg_strtou64(optarg);
				break;
			default:
				print_usage();
		}
	}
	set_argv0(av);
	ac = ac - optind;
	if (check_argc_exact(ac, 1) || block == ~0ULL)
		print_usage();

	info = open_ctree_fs_info(av[optind], 0, 0, flags);
	if (!info) {
		fprintf(stderr, "unable to open %s\n", av[optind]);
		exit(1);
	}

	root = info->fs_root;
	if (!root) {
		fprintf(stderr, "unable to open %s\n", av[optind]);
		exit(1);
	}

	len = read_block(root, block, &eb);
	if (eb->len != len) {
		fprintf(stderr, "length mismatch: %u %d\n", eb->len, len);
		return 1;
	}

	if (flags & OPEN_CTREE_WRITES) {
		char buf[4];
		int ret;
		fprintf(stderr, "*** THIS MAY CORRUPT YOUR FILE SYSTEM ***\n");
		fprintf(stderr, "*** Do you want to write logical block %llu "
			"on device %s ?\n", block, av[optind]);
		fprintf(stderr, "*** Type upper case \"yes\" to continue: ");
		memset(buf, 0, 4);
		ret = read(fileno(stderr), buf, 3);
		if (strcmp(buf, "YES")) {
			fprintf(stderr, "*** Aborted.\n");
			goto out;
		}
		fprintf(stderr, "*** Writing block ... ");
		if (fread(&eb->data, len, 1, stdin) < 1) {
			fprintf(stderr, "failed to read %d bytes\n", len);
			return 1;
		}
		btrfs_set_header_flag(eb, BTRFS_HEADER_FLAG_WRITTEN);
		csum_tree_block(root, eb, 0);
		if (write_and_map_eb(trans, root, eb))
			fprintf(stderr, "error writing block %llu\n", block);
		else
			fprintf(stderr, " done.\n");
	} else if (fwrite(&eb->data, len, 1, stdout) < 1)
		fprintf(stderr, "failed to write %d bytes\n", len);

out:
	free_extent_buffer(eb);
	return close_ctree(root);
}

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
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "version.h"
#include "utils.h"
#include "volumes.h"

static int print_usage(void)
{
	fprintf(stderr, "usage: btrfs-search-metadata [options] device\n");
	fprintf(stderr, "\t-k <objid>/<type>/<offset>: search for given key\n");
	fprintf(stderr, "\t-g <generation>: search for given generation (transid)\n");
	fprintf(stderr, "\t-t <tree-id>: search for given tree\n");
	fprintf(stderr, "\t-l <level>: search for node level (0=leaf)\n");
	fprintf(stderr, "\t-L: print full listing of matching leaf/node contents\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

int bin_search(struct extent_buffer *eb, struct btrfs_key *key,
	       int level, int *slot);

static int do_one_block(struct btrfs_root *root, u64 block_nr, u64 tree_id,
			u64 gen_id, int level, struct btrfs_key *key, int brief)
{
	struct extent_buffer *leaf;
	int ret;
	int slot = -1;
	struct btrfs_disk_key disk_key;

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

	ret = btrfs_is_leaf(leaf);
	if (tree_id != 0 && tree_id != btrfs_header_owner(leaf))
		goto out;
	if (gen_id != 0 && gen_id != btrfs_header_generation(leaf))
		goto out;
	if (level != -1 && level != (int)btrfs_header_level(leaf))
		goto out;

	if (key && key->type != 0ULL) {
		if (bin_search(leaf, key, btrfs_header_level(leaf), &slot))
			goto out;
	}

	if (brief)
		printf("%s %llu level %u items %d free %lu generation %llu owner %llu\n",
		       (ret ? "leaf" : "node"),
		       (unsigned long long)btrfs_header_bytenr(leaf),
		       btrfs_header_level(leaf),
		       btrfs_header_nritems(leaf),
		       (ret ? btrfs_leaf_free_space(root, leaf) :
			(unsigned long)BTRFS_NODEPTRS_PER_BLOCK(root) -
			btrfs_header_nritems(leaf)),
		       (u64)btrfs_header_generation(leaf),
		       (u64)btrfs_header_owner(leaf));
	else
		btrfs_print_tree(root, leaf, 0);

	if (key->objectid != 0ULL) {
		btrfs_cpu_key_to_disk(&disk_key, key);
		printf("\t");
		btrfs_print_key(&disk_key);
		printf(" found @ slot %d in %s %llu\n", slot,
		       (ret ? "leaf" : "node"),
		       (unsigned long long)btrfs_header_bytenr(leaf));
	}
out:
	free_extent_buffer(leaf);
	return ret;
}

static int walk_metadata(struct btrfs_fs_info *info, u64 tree_id, u64 gen_id,
			 int level, struct btrfs_key *key, int brief)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	struct btrfs_root *root = info->tree_root;
	int ret = 0;
	u64 ofs;
	for (ce = first_cache_extent(&info->mapping_tree.cache_tree);
	     ce;
	     ce = next_cache_extent(ce)) {
		map = container_of(ce, struct map_lookup, ce);
		if (!(map->type & BTRFS_BLOCK_GROUP_METADATA))
			continue;
		for (ofs = 0; ofs < map->ce.size; ) {
			int rv = do_one_block(root, map->ce.start + ofs,
					      tree_id, gen_id, level,
					      key, brief);
			if (rv == 1)
				ofs += root->leafsize;
			else if (rv == 0)
				ofs += root->nodesize;
			else
				ofs += (root->leafsize < root->nodesize ?
					root->leafsize : root->nodesize);
		}
	}
	return ret;
}

static int parse_key(char *arg, struct btrfs_key *bk)
{
	char *p1, *p2;
	p1 = strchr(arg, '/');
	if (!p1)
		goto bad;
	p2 = strchr(p1+1, '/');
	if (!p2)
		goto bad;
	*p1 = *p2 = '\0';
	bk->objectid = arg_strtou64(arg);
	bk->type = arg_strtou64(p1 + 1);
	bk->offset = arg_strtou64(p2 + 1);
	return 0;

bad:
	fprintf(stderr, "Invalid format for key: %s, should be <objid>/<key>/<ofs>\n",
		arg);
	return 1;
}

int main(int ac, char **av)
{
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	u64 tree_id = 0ULL;
	u64 gen_id = 0ULL;
	int brief = 1;
	int level = -1;
	struct btrfs_key search_key = { 0ULL, 0, 0ULL };

	radix_tree_init();

	while(1) {
		int c;
		c = getopt(ac, av, "Lk:t:g:l:");
		if (c < 0)
			break;
		switch(c) {
			case 'k':
				if (parse_key(optarg, &search_key))
					return 1;
				break;
			case 't':
				tree_id = arg_strtou64(optarg);
				break;
			case 'g':
				gen_id = arg_strtou64(optarg);
				break;
			case 'l':
				level = arg_strtou64(optarg);
				break;
			case 'L':
				brief = 0;
				break;
			default:
				print_usage();
		}
	}
	set_argv0(av);
	ac = ac - optind;
	if (check_argc_exact(ac, 1))
		print_usage();

	info = open_ctree_fs_info(av[optind], 0, 0, OPEN_CTREE_PARTIAL);
	if (!info) {
		fprintf(stderr, "unable to open %s\n", av[optind]);
		exit(1);
	}

	root = info->fs_root;
	if (!root) {
		fprintf(stderr, "unable to open %s\n", av[optind]);
		exit(1);
	}

	walk_metadata(info, tree_id, gen_id, level, &search_key, brief);

	return close_ctree(root);
}

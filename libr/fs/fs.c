/* radare - LGPL - Copyright 2011 pancake<nopcode.org> */

#include <r_fs.h>
#include "../config.h"

static RFSPlugin *fs_static_plugins[] = { R_FS_STATIC_PLUGINS };

/* lifecycle */
// TODO: needs much more love
R_API RFS *r_fs_new () {
	int i;
	RFSPlugin *static_plugin;
	RFS *fs = R_NEW (RFS);
	if (fs) {
		fs->roots = r_list_new ();
		fs->roots->free = (RListFree)r_fs_root_free;
		fs->plugins = r_list_new ();
		// XXX fs->roots->free = r_fs_plugin_free;
		for (i=0; fs_static_plugins[i]; i++) {
			static_plugin = R_NEW (RFSPlugin);
			memcpy (static_plugin, fs_static_plugins[i], sizeof (RFSPlugin));
			r_fs_add (fs, static_plugin);
		}
	}
	return fs;
}

R_API RFSPlugin *r_fs_plugin_get (RFS *fs, const char *name) {
	RListIter *iter;
	RFSPlugin *p;
	r_list_foreach (fs->plugins, iter, p) {
		if (!strcmp (p->name, name))
			return p;
	}
	return NULL;
}

R_API void r_fs_free (RFS* fs) {
	r_list_free (fs->plugins);
	r_list_free (fs->roots);
	free (fs);
}

/* plugins */

R_API void r_fs_add (RFS *fs, RFSPlugin *p) {
	// find coliding plugin name
	if (p) {
		if (p->init)
			p->init ();
	}
	r_list_append (fs->plugins, p);
}

R_API void r_fs_del (RFS *fs, RFSPlugin *p) {
	// TODO: implement
}

/* mountpoint */

R_API RFSRoot *r_fs_mount (RFS* fs, const char *fstype, const char *path, ut64 delta) {
	RFSPlugin *p;
	RFSRoot *root;

	if (path[0] != '/') {
		eprintf ("r_fs_mount: invalid mountpoint\n");
		return NULL;
	}
	p = r_fs_plugin_get (fs, fstype);
	if (p != NULL) {
		root = r_fs_root_new (path, delta);
		root->p = p;
		//memcpy (&root->iob, &fs->iob, sizeof (root->iob));
		root->iob = fs->iob;
		p->mount (root);
		r_list_append (fs->roots, root);
		eprintf ("Mounted %s on %s at 0x%llx\n", fstype, path, 0LL);
	} else eprintf ("r_fs_mount: Invalid filesystem type\n");
	return root;
}

static inline int r_fs_match (const char *root, const char *path) {
	return (!strncmp (path, root, strlen (path)));
}

R_API int r_fs_umount (RFS* fs, const char *path) {
        RFSRoot *root;
	RListIter *iter;
        r_list_foreach (fs->roots, iter, root) {
		if (r_fs_match (path, root->path)) {
			r_list_delete (fs->roots, iter);
			return R_TRUE;
		}
        }
        return R_FALSE;
}

R_API RFSRoot *r_fs_root (RFS *fs, const char *path) {
        RFSRoot *root;
	RListIter *iter;
        r_list_foreach (fs->roots, iter, root) {
		if (r_fs_match (path, root->path))
			return root;
        }
	return NULL;
}

/* filez */
R_API RFSFile *r_fs_open (RFS* fs, const char *p) {
	char *path = strdup (p);
	r_str_chop_path (path);
	RFSRoot *root = r_fs_root (fs, path);
	if (root && root->p && root->p->open) {
		RFSFile *f = root->p->open (root, path+strlen (root->path));
		free (path);
		return f;
	} else eprintf ("r_fs_open: null root->p->open\n");
	free (path);
        return NULL;
}

// TODO: close or free?
R_API void r_fs_close (RFS* fs, RFSFile *file) {
	if (fs && file && file->p && file->p->close)
		file->p->close (file);
}

R_API int r_fs_read (RFS* fs, RFSFile *file, ut64 addr, int len) {
	if (len<1) {
		eprintf ("r_fs_read: too short read\n");
	} else
	if (fs && file) {
		free (file->data);
		file->data = malloc (len+1);
		if (file->p && file->p->read) {
			file->p->read (file, addr, len);
			return R_TRUE;
		} else eprintf ("r_fs_read: file->p->read is null\n");
	}
	return R_FALSE;
}

R_API RList *r_fs_dir(RFS* fs, const char *p) {
	if (fs) {
		char *path = strdup (p);
		r_str_chop_path (path);
		RFSRoot *root = r_fs_root (fs, path);
		if (root) {
			const char *dir = path + strlen (root->path);
			if (!*dir) dir = "/";
			if (root) {
				free (path);
				return root->p->dir (root, dir);
			}
		}
		free (path);
		eprintf ("r_fs_dir: error, path %s is not mounted\n", path);
	}
	return NULL;
}

R_API RFSFile *r_fs_slurp(RFS* fs, const char *path) {
	RFSFile *file = NULL;
	RFSRoot *root = r_fs_root (fs, path);
	if (root && root->p) {
		if (root->p->open && root->p->read && root->p->close) {
			file = root->p->open (root, path);
			if (file) {
				root->p->read (file, 0, file->size); //file->data, file->size);
			} else eprintf ("r_fs_slurp: cannot open file\n");
		} else {
			if (root->p->slurp)
				return root->p->slurp (root, path);
			else eprintf ("r_fs_slurp: null root->p->slurp\n");
		}
	}
	return file;
}

// TODO: move into grubfs
#include "p/grub/include/grubfs.h"
RList *list = NULL;
static int parhook (struct grub_disk *disk, struct grub_partition *par) {
	RFSPartition *p = r_fs_partition_new (r_list_length (list), par->start*512, 512*par->len);
	p->type = par->msdostype;
	r_list_append (list, p);
	return 0;
}

R_API RList *r_fs_partitions (RFS *fs, const char *ptype, ut64 delta) {
	struct grub_partition_map *gpm = NULL;
	if (!strcmp (ptype, "msdos"))
		gpm = &grub_msdos_partition_map;
	else if (!strcmp (ptype, "apple"))
		gpm = &grub_apple_partition_map;
	else if (!strcmp (ptype, "sun"))
		gpm = &grub_sun_partition_map;
	else if (!strcmp (ptype, "sunpc"))
		gpm = &grub_sun_pc_partition_map;
	else if (!strcmp (ptype, "amiga"))
		gpm = &grub_amiga_partition_map;
	else if (!strcmp (ptype, "bsdlabel"))
		gpm = &grub_bsdlabel_partition_map;
// XXX: In BURG all bsd partition map are in bsdlabel
//	else if (!strcmp (ptype, "openbsdlabel"))
//		gpm = &grub_openbsdlabel_partition_map;
//	else if (!strcmp (ptype, "netbsdlabel"))
//		gpm = &grub_netbsdlabel_partition_map;
//	else if (!strcmp (ptype, "acorn"))
//		gpm = &grub_acorn_partition_map;
	else if (!strcmp (ptype, "gpt"))
		gpm = &grub_gpt_partition_map;

	if (gpm) {
		list = r_list_new ();
		list->free = (RListFree)r_fs_partition_free;
		struct grub_disk *disk = grubfs_disk (&fs->iob);
		gpm->iterate (disk, parhook, 0);
		return list;
	}
	eprintf ("Unknown partition type '%s'. Supported types:\n"
		"  msdos, apple, sun, sunpc, amiga, bsdlabel, acorn, gpt", ptype);
	return NULL;
}

R_API int r_fs_prompt (RFS *fs, char *root) {
	char buf[1024];
	char path[1024];
	char str[2048];
	char *input;
	RList *list;
	RListIter *iter;
	RFSFile *file;

	r_str_chop_path (root);
	strncpy (path, root, sizeof (path)-1);

	for (;;) {
		printf (Color_MAGENTA"%s> "Color_RESET, path);
		fflush (stdout);
		fgets (buf, sizeof (buf)-1, stdin);
		if (feof (stdin)) break;
		buf[strlen (buf)-1] = '\0';
		if (!strcmp (buf, "q") || !strcmp (buf, "exit"))
			return R_TRUE;
		else if (!strcmp (buf, "ls")) {
			list = r_fs_dir (fs, path);
			if (list) {
				r_list_foreach (list, iter, file)
					printf ("%c %s\n", file->type, file->name);
				r_list_free (list);
			} else printf ("Unknown path\n");
		} else if (!strncmp (buf, "cd ", 3)) {
			input = buf+3;
			while (input[0] == ' ')
				input++;
			strncpy (str, path, sizeof(str)-1);
			if (input[0] == '/')
				strncpy (path, root, sizeof (path)-1);
			strcat (path, "/");
			strcat (path, input);
			r_str_chop_path (path);
			if (strlen (path) < strlen (root))
				strncpy (path, root, sizeof (path)-1);
			list = r_fs_dir (fs, path);
			if (!r_list_empty (list))
				r_list_free (list);
			else {
				strncpy (path, str, sizeof (path)-1);
				printf ("Unknown path\n");
			}
		} else if (!strncmp (buf, "cat ", 4)) {
			input = buf+3;
			while (input[0] == ' ')
				input++;
			if (input[0] == '/')
				strncpy (str, root, sizeof (str)-1);
			else
				strncpy (str, path, sizeof (str)-1);
			strcat (str, "/");
			strcat (str, input);
			file = r_fs_open (fs, str);
			if (file) {
				r_fs_read (fs, file, 0, file->size);
				write (1, file->data, file->size);
				r_fs_close (fs, file);
			} else printf ("Cannot open file\n");
		} else if (!strncmp (buf, "get ",4)){
			input = buf+3;
			while (input[0] == ' ')
				input++;
			if (input[0] == '/')
				strncpy (str, root, sizeof (str)-1);
			else
				strncpy (str, path, sizeof (str)-1);
			strcat (str, "/");
			strcat (str, input);
			file = r_fs_open (fs, str);
			if (file) {
				r_fs_read (fs, file, 0, file->size);
				r_file_dump (input, file->data, file->size);
				r_fs_close (fs, file);
			} else printf ("Cannot open file\n");
		} else if (!strcmp (buf, "help") || !strcmp (buf, "?")) {
			printf(
			"Commands:\n"
			" ls          ; list current directory\n"
			" cd path     ; change current directory\n"
			" cat file    ; print contents of file\n"
			" q/exit      ; leave prompt mode\n"
			" ?/help      ; show this help\n"
			);
		} else {
			printf ("Unknown command %s\n", buf);
		}
	}
	clearerr (stdin);
	printf ("\n");
	return R_TRUE;
}

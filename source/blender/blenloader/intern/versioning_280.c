/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/blenloader/intern/versioning_280.c
 *  \ingroup blenloader
 */

/* allow readfile to use deprecated functionality */
#define DNA_DEPRECATED_ALLOW

#include "DNA_object_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_genfile.h"

#include "BKE_collection.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BLO_readfile.h"
#include "readfile.h"

#include "MEM_guardedalloc.h"


/**
 * \brief Before lib-link versioning for new workspace design.
 *
 * Adds a workspace for each screen of the old file and adds the needed workspace-layout to wrap the screen.
 * Rest of the conversion is done in #do_version_workspaces_after_lib_link.
 *
 * Note that some of the created workspaces might be deleted again in case of reading the default startup.blend.
 */
static void do_version_workspaces_before_lib_link(Main *main)
{
	BLI_assert(BLI_listbase_is_empty(&main->workspaces));

	for (bScreen *screen = main->screen.first; screen; screen = screen->id.next) {
		WorkSpace *ws = BKE_workspace_add(main, screen->id.name + 2);
		ScreenLayoutData layout_data = BKE_screen_layout_data_get(screen);
		WorkSpaceLayoutType *layout_type = BKE_workspace_layout_type_add(ws, screen->id.name + 2, layout_data);

		BKE_workspace_active_layout_type_set(ws, layout_type);

		/* For compatibility, the workspace should be activated that represents the active
		 * screen of the old file. This is done in blo_do_versions_after_linking_270. */
	}

	for (wmWindowManager *wm = main->wm.first; wm; wm = wm->id.next) {
		for (wmWindow *win = wm->windows.first; win; win = win->next) {
			win->workspace_hook = BKE_workspace_hook_new();
		}
	}
}

/**
 * \brief After lib-link versioning for new workspace design.
 *
 *  *  Active screen isn't stored directly in window anymore, but in the active workspace.
 *     We already created a new workspace for each screen in #do_version_workspaces_before_lib_link,
 *     here we need to find and activate the workspace that contains the active screen of the old file.
 *  *  Active scene isn't stored in screen anymore, but in window.
 */
static void do_version_workspaces_after_lib_link(Main *main)
{
	for (wmWindowManager *wm = main->wm.first; wm; wm = wm->id.next) {
		for (wmWindow *win = wm->windows.first; win; win = win->next) {
			bScreen *screen = win->screen;
			WorkSpace *workspace = BLI_findstring(&main->workspaces, screen->id.name + 2, offsetof(ID, name) + 2);
			ListBase *layout_types = BKE_workspace_layout_types_get(workspace);
			ListBase *layouts = BKE_workspace_hook_layouts_get(win->workspace_hook);
			WorkSpaceLayout *layout = BKE_workspace_layout_add_from_type(workspace, layout_types->first, screen);

			BLI_assert(BLI_listbase_count(layout_types) == 1);
			BLI_addhead(layouts, layout);
			BKE_workspace_active_layout_set(workspace, layout); /* TODO */
			BKE_workspace_active_set(win->workspace_hook, workspace);
			win->scene = screen->scene;
			BKE_workspace_render_layer_set(workspace, BKE_scene_layer_render_active(win->scene));

			/* Deprecated from now on! */
			win->screen = NULL;
			screen->scene = NULL;
		}
	}
}

void do_versions_after_linking_280(FileData *fd, Main *main)
{
	if (!MAIN_VERSION_ATLEAST(main, 280, 0)) {
		for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
			/* since we don't have access to FileData we check the (always valid) first render layer instead */
			if (scene->render_layers.first == NULL) {
				SceneCollection *sc_master = BKE_collection_master(scene);
				BLI_strncpy(sc_master->name, "Master Collection", sizeof(sc_master->name));

				SceneCollection *collections[20] = {NULL};
				bool is_visible[20];

				int lay_used = 0;
				for (int i = 0; i < 20; i++) {
					char name[MAX_NAME];

					BLI_snprintf(name, sizeof(collections[i]->name), "%d", i + 1);
					collections[i] = BKE_collection_add(scene, sc_master, name);

					is_visible[i] = (scene->lay & (1 << i));
				}

				for (Base *base = scene->base.first; base; base = base->next) {
					lay_used |= base->lay & ((1 << 20) - 1); /* ignore localview */

					for (int i = 0; i < 20; i++) {
						if ((base->lay & (1 << i)) != 0) {
							BKE_collection_object_add(scene, collections[i], base->object);
						}
					}
				}

				scene->active_layer = 0;

				if (!BKE_scene_uses_blender_game(scene)) {
					for (SceneRenderLayer *srl = scene->r.layers.first; srl; srl = srl->next) {

						SceneLayer *sl = BKE_scene_layer_add(scene, srl->name);
						BKE_scene_layer_engine_set(sl, scene->r.engine);

						if (srl->mat_override) {
							BKE_collection_override_datablock_add((LayerCollection *)sl->layer_collections.first, "material", (ID *)srl->mat_override);
						}

						if (srl->light_override && BKE_scene_uses_blender_internal(scene)) {
							/* not sure how we handle this, pending until we design the override system */
							TODO_LAYER_OVERRIDE;
						}

						if (srl->lay != scene->lay) {
							/* unlink master collection  */
							BKE_collection_unlink(sl, sl->layer_collections.first);

							/* add new collection bases */
							for (int i = 0; i < 20; i++) {
								if ((srl->lay & (1 << i)) != 0) {
									BKE_collection_link(sl, collections[i]);
								}
							}
						}

						/* for convenience set the same active object in all the layers */
						if (scene->basact) {
							sl->basact = BKE_scene_layer_base_find(sl, scene->basact->object);
						}

						/* TODO: passes, samples, mask_layesr, exclude, ... */
					}

					if (BLI_findlink(&scene->render_layers, scene->r.actlay)) {
						scene->active_layer = scene->r.actlay;
					}
				}

				SceneLayer *sl = BKE_scene_layer_add(scene, "Viewport");

				/* In this particular case we can safely assume the data struct */
				LayerCollection *lc = ((LayerCollection *)sl->layer_collections.first)->layer_collections.first;
				for (int i = 0; i < 20; i++) {
					if (!is_visible[i]) {
						lc->flag &= ~COLLECTION_VISIBLE;
					}
					lc = lc->next;
				}

				/* but we still need to make the flags synced */
				BKE_scene_layer_base_flag_recalculate(sl);

				/* convert active base */
				if (scene->basact) {
					sl->basact = BKE_scene_layer_base_find(sl, scene->basact->object);
				}

				/* convert selected bases */
				for (Base *base = scene->base.first; base; base = base->next) {
					Base *ob_base = BKE_scene_layer_base_find(sl, base->object);
					if ((base->flag & SELECT) != 0) {
						if ((ob_base->flag & BASE_SELECTABLED) != 0) {
							ob_base->flag |= BASE_SELECTED;
						}
					}
					else {
						ob_base->flag &= ~BASE_SELECTED;
					}
				}

				/* TODO: copy scene render data to layer */

				/* Cleanup */
				for (int i = 0; i < 20; i++) {
					if ((lay_used & (1 << i)) == 0) {
						BKE_collection_remove(scene, collections[i]);
					}
				}

				/* remove bases once and for all */
				for (Base *base = scene->base.first; base; base = base->next) {
					id_us_min(&base->object->id);
				}
				BLI_freelistN(&scene->base);
				scene->basact = NULL;
			}
		}
	}

	{
		/* New workspace design */
		if (!DNA_struct_find(fd->filesdna, "WorkSpace")) {
			do_version_workspaces_after_lib_link(main);
		}
	}
}

static void blo_do_version_temporary(Main *main)
{
	BKE_scene_layer_doversion_update(main);
}

void blo_do_versions_280(FileData *fd, Library *UNUSED(lib), Main *main)
{
	if (!MAIN_VERSION_ATLEAST(main, 280, 0)) {
		if (!DNA_struct_elem_find(fd->filesdna, "Scene", "ListBase", "render_layers")) {
			for (Scene *scene = main->scene.first; scene; scene = scene->id.next) {
				/* Master Collection */
				scene->collection = MEM_callocN(sizeof(SceneCollection), "Master Collection");
				BLI_strncpy(scene->collection->name, "Master Collection", sizeof(scene->collection->name));
			}
		}

		/* temporary validation of 280 files for layers */
		blo_do_version_temporary(main);
	}

	{
		/* New workspace design */
		if (!DNA_struct_find(fd->filesdna, "WorkSpace")) {
			do_version_workspaces_before_lib_link(main);
		}
	}
}

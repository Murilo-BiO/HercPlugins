/**
 * This plugin will allow you to block items from being dropped by monsters
 *
 * You can specify the monster type and some exceptions.
 * It's like a `mob_item_ratio.txt` but it is to remove the drop.
 *
 * Copyright (C) 2017  ReburnRO Dev Team
 */

#include "common/hercules.h"

#include "common/conf.h"
#include "common/db.h"
#include "common/HPMi.h"
#include "common/memmgr.h"
#include "common/mmo.h"
#include "common/nullpo.h"
#include "common/showmsg.h"
#include "common/strlib.h"
#include "common/utils.h"

#include "map/itemdb.h"
#include "map/mob.h"
#include "map/status.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h"

#include <stdio.h>
#include <stdlib.h>

HPExport struct hplugin_info pinfo = {
	"itemdroplock", // Plugin name
	SERVER_TYPE_MAP, // Which server types this plugin works with
	"1.0",           // Plugin version
	HPM_VERSION      // HPM Version (don't change, macro is automatically updated)
};

/**
 * The path and name of the config file
 *
 * @const (const char *)
 */
const char *filename = "src/plugins/conf/itemdroplock.conf";

/**
 * Struct of blocked items.
 */
struct locked_item {
	bool normal;
	bool boss;
	bool mvp;
	bool except[MAX_MOB_DB];
};

/**
 * The "database" of blocked drops.
 * 
 * @var (struct locked_item)[]
 */
static struct locked_item items[MAX_ITEMDB];

/**
 * Reads the config file to load all blocked drops.
 * 
 * @return int
 */
int read_conf(void)
{
	struct config_t drop_db_conf;
	struct config_setting_t *drop_db, *drop;
	struct item_data *id;
	bool duplicate[MAX_ITEMDB];
	int i = 0;

	if (!libconfig->load_file(&drop_db_conf, filename))
		return 0;

	if ((drop_db = libconfig->setting_get_member(drop_db_conf.root, "drop_block_db")) == NULL) {
		ShowError("DropLockPlugin::read_conf: can't read %s\n", filename);
		return 0;
	}

	memset(&duplicate, false, sizeof(duplicate));

	while ((drop = libconfig->setting_get_elem(drop_db, i++)) != NULL) {
		const char *aegisname;

		if (config_setting_is_group(drop)) {
			if (!libconfig->setting_lookup_string(drop, "Item", &aegisname)) {
				ShowError("DropLockPlugin: Item name not found for entry #%d in %s, skipping...\n", i, filename);
				continue;
			}
		} else {
			aegisname = libconfig->setting_get_string(drop);
		}

		if ((id = itemdb->name2id(aegisname)) == NULL) {
			ShowError("DropLockPlugin: Invalid AegisName for entry #%d in %s\n", i, filename);
			continue;
		}

		if (duplicate[id->nameid]) {
			ShowError("DropLockPlugin: Duplicate entry for %s, entry %d in %s\n", id->name, i, filename);
			continue;
		} else {
			duplicate[id->nameid] = true;
			items[id->nameid].normal = true;
			items[id->nameid].boss = true;
			items[id->nameid].mvp = true;
			memset(&items[id->nameid].except, false, sizeof(items[id->nameid].except));
			
			if (config_setting_is_group(drop)) {
				struct config_setting_t *mobs, *type;

				if ((type = libconfig->setting_get_member(drop, "Normal")) != NULL) {
					items[id->nameid].normal = false;
					items[id->nameid].normal = libconfig->setting_get_bool_real(type);
				}

				if ((type = libconfig->setting_get_member(drop, "Boss")) != NULL) {
					items[id->nameid].boss = false;
					items[id->nameid].boss = libconfig->setting_get_bool_real(type);
				}

				if ((type = libconfig->setting_get_member(drop, "Mvp")) != NULL) {
					items[id->nameid].mvp = false;
					items[id->nameid].mvp = libconfig->setting_get_bool_real(type);
				}
	
				if ((mobs = libconfig->setting_get_member(drop, "Except")) != NULL) {
					int count = libconfig->setting_length(mobs);
					bool mobduplicate[MAX_MOB_DB];
					memset(&mobduplicate, false, sizeof(mobduplicate));
					for (int j = 0; j < count; j++) {
						const char *mobname = libconfig->setting_get_string_elem(mobs, j);
						struct mob_db *monster = mob->db(mob->db_searchname(mobname));
	
						if (monster == mob->dummy) {
							ShowError("DropLockPlugin: Invalid mob '%s' for item '%s' in %s\n", mobname, aegisname, filename);
							continue;
						}
						
						if (mobduplicate[monster->mob_id]) {
							ShowError("DropLockPlugin: Duplicate mob '%s' for item '%s' in %s\n", monster->name, aegisname, filename);
							continue;
						} else {
							mobduplicate[monster->mob_id] = true;
							items[id->nameid].except[monster->mob_id] = true;
						}
					}
				}
			}
		}
	}

	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", i - 1, filename);
	return 1;
}

/**
 * Remove the given blocked item from monsters (after mob_db and item_db have been read)
 * 
 * @return void
 */
void block_drops(void)
{
	int i;

	for (i = 0; i < MAX_MOB_DB; i++) {
		struct mob_db *monster = mob->db(i);

		if (monster == mob->dummy)
			continue;
		
		for (int j = 0; j < MAX_MOB_DROP; j++) {
			int it;

			if ((it = monster->dropitem[j].nameid) > 0) {
				int k;
				struct item_data *item = itemdb->search(it);
				ARR_FIND(0, MAX_SEARCH, k, mob->db(item->mob[k].id)->mob_id == monster->mob_id);

				if (items[it].except[monster->mob_id]) {
					continue;
				} else if (monster->mexp > 0 && !items[it].mvp) {
					continue;
				} else if (monster->mexp <= 0 && (monster->status.mode & MD_BOSS) && !items[it].boss) {
					continue;
				} else if (monster->mexp <= 0 && ~(monster->status.mode & MD_BOSS) && !items[it].normal) {
					continue;
				}

				if (k < MAX_SEARCH) {
					item->mob[k].chance = 0;
					if (items[it].normal && items[it].boss && items[it].mvp) {
						item->maxchance = 0;
					}
				}
				monster->dropitem[j].nameid = 0;
				monster->dropitem[j].p = 0;
			}
		}

		for (int j = 0; j < MAX_MVP_DROP; j++) {
			int it;

			if (monster->mexp <= 0)
				break;

			if ((it = monster->mvpitem[j].nameid) > 0) {
				if (items[it].except[monster->mob_id]) {
					continue;
				} else if (!items[it].mvp) {
					continue;
				} else {
					monster->mvpitem[j].nameid = 0;
					monster->mvpitem[j].p = 0;
				}
			}
		}
	}
}

/**
 * Resets the array of blocked items
 * 
 * @return void
 */
void destroy_block_drop_db(void)
{
	for (int i = 0; i < ARRAYLENGTH(items); i++) {
		items[i].normal = false;
		items[i].boss = false;
		items[i].mvp = false;
		memset(&items[i].except, false, sizeof(items[i].except));
	}
}

/**
 * Post Hook of mob_load
 * 
 * It will call the function to remove the blocked items
 * 
 * @param bool minimal
 * @return void
 */
void mob_load_post(bool minimal)
{
	block_drops();
}

/**
 * Post Hook of itemdb_reload
 * 
 * It will call the function to reset and read the "db"
 * 
 * @return void
 */
void itemdb_reload_post(void)
{
	destroy_block_drop_db();
	read_conf();
}

/**
 * Atcommand to reload the blocked drops
 * 
 * Usage: @reloadblockeddrops
 */
ACMD(reloadlockeddrops)
{
	destroy_block_drop_db();
	read_conf();
	mob->reload();
	clif->message(fd, "Locked Drops have been reloaded");
	
	return true;
}

/* Server Startup */
HPExport void plugin_init(void)
{
	if (SERVER_TYPE == SERVER_TYPE_MAP) {
		addHookPost(mob, load, mob_load_post);
		addHookPost(itemdb, reload, itemdb_reload_post);

		addAtcommand("reloadlockeddrops", reloadblockeddrops);
	}
}

HPExport void server_online(void)
{
	if (SERVER_TYPE == SERVER_TYPE_MAP) {
		read_conf();
		block_drops();
	}
}

/* run when server is shutting down */
HPExport void plugin_final(void)
{
	destroy_block_drop_db();
}

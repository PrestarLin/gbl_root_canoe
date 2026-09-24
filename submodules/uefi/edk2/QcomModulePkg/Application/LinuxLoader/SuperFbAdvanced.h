/*
 * Advanced submenu for the super-fastboot boot menu.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_ADVANCED_H__
#define __SUPER_FB_ADVANCED_H__

/*
 * Run the Advanced submenu:
 *
 *   Advanced
 *    `- USB Mass Storage
 *        |- Export a partition   -> LUN picker -> partition picker -> mode
 *        `- Export a LUN         -> LUN picker -> mode
 *
 * "mode" is the Read only / Read Write chooser shown before every export.
 * Partitions are exported behind a RAM-backed fake GPT (always write
 * protected); a whole LUN is exported one-to-one, optionally forced
 * read-only. Returns when the user backs out.
 */
VOID
SfbRunAdvancedMenu (VOID);

#endif /* __SUPER_FB_ADVANCED_H__ */

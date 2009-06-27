#ifndef foopulseextdevicemanagerhfoo
#define foopulseextdevicemanagerhfoo

/***
  This file is part of PulseAudio.

  Copyright 2008 Lennart Poettering
  Copyright 2009 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/context.h>
#include <pulse/version.h>

/** \file
 *
 * Routines for controlling module-stream-restore
 */

PA_C_DECL_BEGIN

/** Stores information about one device in the device database that is
 * maintained by module-device-manager. \since 0.9.17 */
typedef struct pa_ext_device_manager_info {
    const char *name;            /**< Identifier string of the device. A string like "sink:" or similar followed by the name of the device. */
    const char *description;     /**< The description of the device when it was last seen, if applicable and saved */
} pa_ext_device_manager_info;

/** Callback prototype for pa_ext_device_manager_test(). \since 0.9.17 */
typedef void (*pa_ext_device_manager_test_cb_t)(
        pa_context *c,
        uint32_t version,
        void *userdata);

/** Test if this extension module is available in the server. \since 0.9.17 */
pa_operation *pa_ext_device_manager_test(
        pa_context *c,
        pa_ext_device_manager_test_cb_t cb,
        void *userdata);

/** Callback prototype for pa_ext_device_manager_read(). \since 0.9.17 */
typedef void (*pa_ext_device_manager_read_cb_t)(
        pa_context *c,
        const pa_ext_device_manager_info *info,
        int eol,
        void *userdata);

/** Read all entries from the device database. \since 0.9.17 */
pa_operation *pa_ext_device_manager_read(
        pa_context *c,
        pa_ext_device_manager_read_cb_t cb,
        void *userdata);

/** Store entries in the device database. \since 0.9.17 */
pa_operation *pa_ext_device_manager_write(
        pa_context *c,
        pa_update_mode_t mode,
        const pa_ext_device_manager_info data[],
        unsigned n,
        int apply_immediately,
        pa_context_success_cb_t cb,
        void *userdata);

/** Delete entries from the device database. \since 0.9.17 */
pa_operation *pa_ext_device_manager_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata);

/** Subscribe to changes in the device database. \since 0.9.17 */
pa_operation *pa_ext_device_manager_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata);

/** Callback prototype for pa_ext_device_manager_set_subscribe_cb(). \since 0.9.17 */
typedef void (*pa_ext_device_manager_subscribe_cb_t)(
        pa_context *c,
        void *userdata);

/** Set the subscription callback that is called when
 * pa_ext_device_manager_subscribe() was called. \since 0.9.17 */
void pa_ext_device_manager_set_subscribe_cb(
        pa_context *c,
        pa_ext_device_manager_subscribe_cb_t cb,
        void *userdata);

PA_C_DECL_END

#endif
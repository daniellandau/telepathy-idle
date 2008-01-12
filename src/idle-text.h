/*
 * This file is part of telepathy-idle
 *
 * Copyright (C) 2006-2007 Collabora Limited
 * Copyright (C) 2006-2007 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __IDLE_TEXT_H__
#define __IDLE_TEXT_H__

#include <glib.h>
#include <dbus/dbus-glib.h>

#include "idle-connection.h"

G_BEGIN_DECLS

void idle_text_decode(const gchar *text, TpChannelTextMessageType *type, gchar **body);
void idle_text_send(GObject *obj, guint type, const gchar *recipient, const gchar *text, IdleConnection *conn, DBusGMethodInvocation *ctx);

G_END_DECLS

#endif


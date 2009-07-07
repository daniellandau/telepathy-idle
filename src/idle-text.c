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

#include "idle-text.h"

#include <time.h>
#include <string.h>

#include <telepathy-glib/text-mixin.h>

#define IDLE_DEBUG_FLAG IDLE_DEBUG_TEXT
#include "idle-ctcp.h"
#include "idle-debug.h"

gboolean idle_text_decode(const gchar *text, TpChannelTextMessageType *type, gchar **body) {
	gchar *tmp = NULL;

	if (text[0] != '\001') {
		*type = TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL;
		tmp = g_strdup(text);
	} else {
		size_t actionlen = strlen("\001ACTION ");
		if (!g_ascii_strncasecmp(text, "\001ACTION ", actionlen)) {
			*type = TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION;
			tmp = g_strndup(text + actionlen, strlen(text + actionlen) - 1);
		} else {
			*body = NULL;
			return FALSE;
		}
	}

	*body = idle_ctcp_kill_blingbling(tmp);
	g_free(tmp);
	return TRUE;
}

/**
 * idle_text_encode_and_split:
 * @type: The type of message as per Telepathy
 * @recipient: The target user or channel
 * @text: The message body
 * @max_msg_len: The maximum length of the message on this server (see also
 *               idle_connection_get_max_message_length())
 * @bodies_out: Location at which to return the human-readable bodies of each
 *              part
 * @error: Location at which to store an error
 *
 * Splits @text as necessary to be able to send it over IRC. IRC messages
 * cannot contain newlines, and have a (server-determined) maximum length.
 *
 * Returns: A list of IRC protocol commands representing @text as best possible.
 */
GStrv
idle_text_encode_and_split(TpChannelTextMessageType type,
		const gchar *recipient,
		const gchar *text,
		gsize max_msg_len,
		GStrv *bodies_out,
		GError **error) {
	GPtrArray *messages;
	GPtrArray *bodies;
	const gchar *remaining_text = text;
	const gchar * const text_end =  text + strlen(text);
	gchar *header;
	const gchar *footer = "";
	gsize max_bytes;

	switch (type) {
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL:
			header = g_strdup_printf("PRIVMSG %s :", recipient);
			break;
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION:
			header = g_strdup_printf("PRIVMSG %s :\001ACTION ", recipient);
			footer = "\001";
			break;
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_NOTICE:
			header = g_strdup_printf("NOTICE %s :", recipient);
			break;
		default:
			IDLE_DEBUG("unsupported message type %u", type);
			g_set_error(error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED, "unsupported message type %u", type);
			return NULL;
	}

	messages = g_ptr_array_new();
	bodies = g_ptr_array_new();
	max_bytes = max_msg_len - (strlen(header) + strlen(footer));

	while (remaining_text < text_end) {
		char *newline = strchr(remaining_text, '\n');
		const char *end_iter;
		char *message;
		int len;

		if (newline != NULL && ((unsigned) (newline - remaining_text)) < max_bytes) {
			/* String up to the next newline is short enough. */
			end_iter = newline;

		} else if ((text_end - remaining_text) > (gint) max_bytes) {
			/* Remaining string is too long; take as many bytes as possible */
			end_iter = remaining_text + max_bytes;
			/* make sure we don't break a UTF-8 code point in half */
			end_iter = g_utf8_find_prev_char (remaining_text, end_iter);
		} else {
			/* Just take it all. */
			end_iter = text_end;
		}

		len = (int)(end_iter - remaining_text);
		message = g_strdup_printf("%s%.*s%s", header, len, remaining_text, footer);
		g_ptr_array_add(messages, message);
		g_ptr_array_add(bodies, g_strndup(remaining_text, len));

		remaining_text = end_iter;
		if (*end_iter == '\n') {
				/* advance over a newline */
				remaining_text++;
		}
	}

	g_assert (remaining_text == text_end);

	g_ptr_array_add(messages, NULL);
	g_ptr_array_add(bodies, NULL);

	if (bodies_out != NULL) {
		*bodies_out = (GStrv) g_ptr_array_free(bodies, FALSE);
	} else {
		g_ptr_array_free(bodies, TRUE);
	}

	g_free(header);
	return (GStrv) g_ptr_array_free(messages, FALSE);
}

void idle_text_send(GObject *obj, guint type, const gchar *recipient, const gchar *text, IdleConnection *conn, DBusGMethodInvocation *context) {
	time_t timestamp = time(NULL);
	GError *error = NULL;
	GStrv messages;
	GStrv bodies;

	if ((recipient == NULL) || (strlen(recipient) == 0)) {
		IDLE_DEBUG("invalid recipient");

		error = g_error_new(TP_ERRORS, TP_ERROR_NOT_AVAILABLE, "invalid recipient");
		dbus_g_method_return_error(context, error);
		g_error_free(error);

		return;
	}

	gsize msg_len = idle_connection_get_max_message_length(conn);
	messages = idle_text_encode_and_split(type, recipient, text, msg_len, &bodies, &error);
	if (messages == NULL) {
		dbus_g_method_return_error(context, error);
		g_error_free(error);
		return;
	}

	for(guint i = 0; messages[i] != NULL; i++) {
		g_assert(bodies[i] != NULL);
		idle_connection_send(conn, messages[i]);

		tp_svc_channel_type_text_emit_sent(obj, timestamp, type, bodies[i]);
	}

	g_strfreev(messages);

	tp_svc_channel_type_text_return_from_send(context);
}


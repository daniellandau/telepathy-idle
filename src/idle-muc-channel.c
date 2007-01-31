/*
 * This file is part of telepathy-idle
 * 
 * Copyright (C) 2006 Nokia Corporation. All rights reserved.
 *
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

#include <glib.h>
#include <dbus/dbus-glib.h>

#include <telepathy-glib/enums.h>
#include <telepathy-glib/interfaces.h>

#define _GNU_SOURCE
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "idle-muc-channel.h"
#include "idle-muc-channel-signals-marshal.h"

#include "idle-muc-channel-glue.h"

#include "idle-connection.h"
#include "idle-handles.h"
#include "idle-handle-set.h"

#include "telepathy-helpers.h"
#include "telepathy-errors.h"

#define IRC_MSG_MAXLEN 510

#define DEBUGSPIKE {g_debug("at %s, line %u", G_STRFUNC, __LINE__);}

G_DEFINE_TYPE(IdleMUCChannel, idle_muc_channel, G_TYPE_OBJECT)

/* signal enum */
enum
{
    CLOSED,
    GROUP_FLAGS_CHANGED,
    LOST_MESSAGE,
    MEMBERS_CHANGED,
    PASSWORD_FLAGS_CHANGED,
    PROPERTIES_CHANGED,
    PROPERTY_FLAGS_CHANGED,
    RECEIVED,
    SEND_ERROR,
    SENT,
	JOIN_READY,
    LAST_SIGNAL
};

/* property enum */
enum
{
	PROP_CONNECTION = 1,
	PROP_OBJECT_PATH,
	PROP_CHANNEL_TYPE,
	PROP_HANDLE_TYPE,
	PROP_HANDLE,
	LAST_PROPERTY_ENUM
};

typedef enum
{
	MUC_STATE_CREATED = 0,
	MUC_STATE_JOINING,
	MUC_STATE_NEED_PASSWORD,
	MUC_STATE_JOINED,
	MUC_STATE_PARTED
} IdleMUCState;

#define LAST_MODE_FLAG_SHIFT (15)

typedef enum
{
	MODE_FLAG_CREATOR = 1,
	MODE_FLAG_OPERATOR_PRIVILEGE = 2,
	MODE_FLAG_VOICE_PRIVILEGE = 4,
	
	MODE_FLAG_ANONYMOUS = 8,
	MODE_FLAG_INVITE_ONLY = 16,
	MODE_FLAG_MODERATED= 32,
	MODE_FLAG_NO_OUTSIDE_MESSAGES = 64,
	MODE_FLAG_QUIET= 128,
	MODE_FLAG_PRIVATE= 256,
	MODE_FLAG_SECRET= 512,
	MODE_FLAG_SERVER_REOP= 1024,
	MODE_FLAG_TOPIC_ONLY_SETTABLE_BY_OPS = 2048,
	
	MODE_FLAG_KEY= 4096,
	MODE_FLAG_USER_LIMIT = 8192,

	MODE_FLAG_HALFOP_PRIVILEGE = 16384,

	LAST_MODE_FLAG_ENUM
} IRCChannelModeFlags;


typedef struct
{
	IRCChannelModeFlags flags;
	guint limit;
	gchar *topic;
	gchar *key;
	guint topic_touched;
	guint topic_toucher;
} IRCChannelModeState;

#define TP_TYPE_PROPERTY_INFO_STRUCT (dbus_g_type_get_struct ("GValueArray", \
      G_TYPE_UINT, \
      G_TYPE_STRING, \
      G_TYPE_STRING, \
      G_TYPE_UINT, \
      G_TYPE_INVALID))
#define TP_TYPE_PROPERTY_INFO_LIST (dbus_g_type_get_collection ("GPtrArray", \
      TP_TYPE_PROPERTY_INFO_STRUCT))

#define TP_TYPE_PROPERTY_VALUE_STRUCT (dbus_g_type_get_struct ("GValueArray", \
      G_TYPE_UINT, \
      G_TYPE_VALUE, \
      G_TYPE_INVALID))
#define TP_TYPE_PROPERTY_VALUE_LIST (dbus_g_type_get_collection ("GPtrArray", \
      TP_TYPE_PROPERTY_VALUE_STRUCT))

#define TP_TYPE_PROPERTY_FLAGS_STRUCT (dbus_g_type_get_struct ("GValueArray", \
      G_TYPE_UINT, \
      G_TYPE_UINT, \
      G_TYPE_INVALID))
#define TP_TYPE_PROPERTY_FLAGS_LIST (dbus_g_type_get_collection ("GPtrArray", \
      TP_TYPE_PROPERTY_FLAGS_STRUCT))	
	
typedef enum
{
	TP_PROPERTY_INVITE_ONLY = 0,
	TP_PROPERTY_LIMIT,
	TP_PROPERTY_LIMITED,
	TP_PROPERTY_MODERATED,
	TP_PROPERTY_PASSWORD,
	TP_PROPERTY_PASSWORD_REQUIRED,
	TP_PROPERTY_PRIVATE,
	TP_PROPERTY_SUBJECT,
	TP_PROPERTY_SUBJECT_TIMESTAMP,
	TP_PROPERTY_SUBJECT_CONTACT,
	LAST_TP_PROPERTY_ENUM
} IdleMUCChannelTPProperty;

typedef struct
{
	const gchar *name;
	GType type;
} TPPropertySignature;

typedef struct
{
	GValue *value;
	guint flags;
} TPProperty;


static const TPPropertySignature property_signatures[] =
{
	{"invite-only", G_TYPE_BOOLEAN},
	{"limit", G_TYPE_UINT},
	{"limited", G_TYPE_BOOLEAN},
	{"moderated", G_TYPE_BOOLEAN},
	{"password", G_TYPE_STRING},
	{"password-required", G_TYPE_BOOLEAN},
	{"private", G_TYPE_BOOLEAN},
	{"subject", G_TYPE_STRING},
	{"subject-timestamp", G_TYPE_UINT},
	{"subject-contact", G_TYPE_UINT},
	{NULL, G_TYPE_NONE}
};

static const gchar *ascii_muc_states[] = 
{
	"MUC_STATE_CREATED",
	"MUC_STATE_JOINING",
	"MUC_STATE_NEED_PASSWORD",
	"MUC_STATE_JOINED",
	"MUC_STATE_PARTED"
};

static void muc_channel_tp_properties_init(IdleMUCChannel *chan);
static void muc_channel_tp_properties_destroy(IdleMUCChannel *chan);
static void change_tp_properties(IdleMUCChannel *chan, const GPtrArray *props);
static void set_tp_property_flags(IdleMUCChannel *chan, const GArray *prop_ids, TpPropertyFlags add, TpPropertyFlags remove);

static guint signals[LAST_SIGNAL] = {0};

/* private structure */
typedef struct _IdleMUCChannelPrivate IdleMUCChannelPrivate;

struct _IdleMUCChannelPrivate
{
	IdleConnection *connection;
	gchar *object_path;
	IdleHandle handle;
	const gchar *channel_name;
	
	IdleHandle own_handle;

	guint recv_id;
	GQueue *pending_messages;

	IdleHandleSet *local_pending;
	IdleHandleSet *remote_pending;
	IdleHandleSet *current_members;

	IdleMUCState state;

	IRCChannelModeState mode_state;

	guint group_flags;
	guint password_flags;
	TPProperty *properties;

	DBusGMethodInvocation *passwd_ctx;
	
	gboolean join_ready;
	gboolean closed;
	
	gboolean dispose_has_run;
};

typedef struct _IdleMUCPendingMessage IdleMUCPendingMessage;

struct _IdleMUCPendingMessage
{
	guint id;

	time_t timestamp;
	IdleHandle sender;

	TpChannelTextMessageType type;

	gchar *text;
};

static void change_group_flags(IdleMUCChannel *chan, guint add, guint delete);
static void change_password_flags(IdleMUCChannel *chan, guint flag, gboolean state);

#define _idle_muc_pending_new() \
	(g_slice_new(IdleMUCPendingMessage))
#define _idle_muc_pending_new0() \
	(g_slice_new0(IdleMUCPendingMessage))

static void _idle_muc_pending_free(IdleMUCPendingMessage *msg)
{
	if (msg->text)
	{
		g_free(msg->text);
	}

	g_slice_free(IdleMUCPendingMessage, msg);
}

#define IDLE_MUC_CHANNEL_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), IDLE_TYPE_MUC_CHANNEL, IdleMUCChannelPrivate))

static void
idle_muc_channel_init (IdleMUCChannel *obj)
{
  IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE (obj);

  priv->pending_messages = g_queue_new();

  priv->group_flags =  0; /* |
	  					TP_CHANNEL_GROUP_FLAG_CAN_REMOVE |
						TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE;*/

  priv->password_flags = 0;
  
  priv->closed = FALSE;
  priv->state = MUC_STATE_CREATED;
  
  priv->mode_state.flags = 0;
  priv->mode_state.topic = NULL;
  priv->mode_state.key = NULL;

  priv->properties = g_new0(TPProperty, LAST_TP_PROPERTY_ENUM);
  muc_channel_tp_properties_init(obj);

  priv->dispose_has_run = FALSE;
}

static void idle_muc_channel_dispose (GObject *object);
static void idle_muc_channel_finalize (GObject *object);

static GObject *idle_muc_channel_constructor(GType type, guint n_props, GObjectConstructParam *props)
{
	GObject *obj;
	IdleMUCChannelPrivate *priv;
	DBusGConnection *bus;
	IdleHandleStorage *handles;
	gboolean valid;

	obj = G_OBJECT_CLASS(idle_muc_channel_parent_class)->constructor(type, n_props, props);
	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(IDLE_MUC_CHANNEL(obj));

	handles = _idle_connection_get_handles(priv->connection);
	valid = idle_handle_ref(handles, TP_HANDLE_TYPE_ROOM, priv->handle);
	g_assert(valid);
	priv->channel_name = idle_handle_inspect(handles, TP_HANDLE_TYPE_ROOM, priv->handle);

	idle_connection_get_self_handle(priv->connection, &(priv->own_handle), NULL);
	valid = idle_handle_ref(handles, TP_HANDLE_TYPE_CONTACT, priv->own_handle);
	g_assert(valid);

	priv->local_pending = idle_handle_set_new(handles, TP_HANDLE_TYPE_CONTACT);
	priv->remote_pending = idle_handle_set_new(handles, TP_HANDLE_TYPE_CONTACT);
	priv->current_members = idle_handle_set_new(handles, TP_HANDLE_TYPE_CONTACT);

	bus = tp_get_bus();
	dbus_g_connection_register_g_object(bus, priv->object_path, obj);
	g_assert(valid);

	return obj;
}

static void idle_muc_channel_get_property(GObject *object, guint property_id, 
										  GValue *value, GParamSpec *pspec)
{
	IdleMUCChannel *chan;
	IdleMUCChannelPrivate *priv;

	g_assert(object != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(object));

	chan = IDLE_MUC_CHANNEL(object);
	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	
	switch (property_id)
	{
		case PROP_CONNECTION:
		{
			g_value_set_object(value, priv->connection);
		}
		break;
		case PROP_OBJECT_PATH:
		{
			g_value_set_string(value, priv->object_path);
		}
		break;
		case PROP_CHANNEL_TYPE:
		{
			g_value_set_string(value, TP_IFACE_CHANNEL_TYPE_TEXT);
		}
		break;
		case PROP_HANDLE_TYPE:
		{
			g_value_set_uint(value, TP_HANDLE_TYPE_ROOM);
		}
		break;
		case PROP_HANDLE:
		{
			g_value_set_uint(value, priv->handle);
		}
		break;
		default:
		{
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		}
		break;
	}
}

static void idle_muc_channel_set_property(GObject *object, guint property_id, const GValue *value,
											GParamSpec *pspec)
{
	IdleMUCChannel *chan = IDLE_MUC_CHANNEL(object);
	IdleMUCChannelPrivate *priv;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));
		
	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	switch (property_id)
	{
		case PROP_CONNECTION:
		{
			priv->connection = g_value_get_object(value);
		}
		break;
		case PROP_OBJECT_PATH:
		{
			if (priv->object_path)
			{
				g_free(priv->object_path);
			}

			priv->object_path = g_value_dup_string(value);
		}
		break;
		case PROP_HANDLE:
		{
			priv->handle = g_value_get_uint(value);
			g_debug("%s: setting handle to %u", G_STRFUNC, priv->handle);
		}
		break;
		default:
		{
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		}
		break;
	}
}

static void
idle_muc_channel_class_init (IdleMUCChannelClass *idle_muc_channel_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (idle_muc_channel_class);
  GParamSpec *param_spec;

  g_type_class_add_private (idle_muc_channel_class, sizeof (IdleMUCChannelPrivate));

  object_class->constructor = idle_muc_channel_constructor;

  object_class->get_property = idle_muc_channel_get_property;
  object_class->set_property = idle_muc_channel_set_property;
  
  object_class->dispose = idle_muc_channel_dispose;
  object_class->finalize = idle_muc_channel_finalize;

  param_spec = g_param_spec_object ("connection", "IdleConnection object",
                                    "The IdleConnection object that owns this "
                                    "MUCChannel object.",
                                    IDLE_TYPE_CONNECTION,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
                                    "The D-Bus object path used for this "
                                    "object on the bus.",
                                    NULL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_string ("channel-type", "Telepathy channel type",
                                    "The D-Bus interface representing the "
                                    "type of this channel.",
                                    NULL,
                                    G_PARAM_READABLE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CHANNEL_TYPE, param_spec);

  param_spec = g_param_spec_uint ("handle-type", "Contact handle type",
                                  "The TpHandleType representing a "
                                  "room handle.",
                                  0, G_MAXUINT32, 0,
                                  G_PARAM_READABLE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_HANDLE_TYPE, param_spec);

  param_spec = g_param_spec_uint ("handle", "Contact handle",
                                  "The IdleHandle representing this room.",
                                  0, G_MAXUINT32, 0,
                                  G_PARAM_CONSTRUCT_ONLY |
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_HANDLE, param_spec);

  signals[CLOSED] =
    g_signal_new ("closed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[GROUP_FLAGS_CHANGED] =
    g_signal_new ("group-flags-changed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__INT_INT,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

  signals[LOST_MESSAGE] =
    g_signal_new ("lost-message",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[MEMBERS_CHANGED] =
    g_signal_new ("members-changed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__STRING_BOXED_BOXED_BOXED_BOXED_INT_INT,
                  G_TYPE_NONE, 7, G_TYPE_STRING, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, G_TYPE_UINT, G_TYPE_UINT);

  signals[PASSWORD_FLAGS_CHANGED] =
    g_signal_new ("password-flags-changed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__INT_INT,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

  signals[PROPERTIES_CHANGED] =
    g_signal_new ("properties-changed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, (dbus_g_type_get_collection ("GPtrArray", (dbus_g_type_get_struct ("GValueArray", G_TYPE_UINT, G_TYPE_VALUE, G_TYPE_INVALID)))));

  signals[PROPERTY_FLAGS_CHANGED] =
    g_signal_new ("property-flags-changed",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, (dbus_g_type_get_collection ("GPtrArray", (dbus_g_type_get_struct ("GValueArray", G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INVALID)))));

  signals[RECEIVED] =
    g_signal_new ("received",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__INT_INT_INT_INT_INT_STRING,
                  G_TYPE_NONE, 6, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);

  signals[SEND_ERROR] =
    g_signal_new ("send-error",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__INT_INT_INT_STRING,
                  G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);

  signals[SENT] =
    g_signal_new ("sent",
                  G_OBJECT_CLASS_TYPE (idle_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  idle_muc_channel_marshal_VOID__INT_INT_STRING,
                  G_TYPE_NONE, 3, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);

  signals[JOIN_READY] =
	  g_signal_new("join-ready",
			  		G_OBJECT_CLASS_TYPE(idle_muc_channel_class),
					G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
					0,
					NULL, NULL,
					idle_muc_channel_marshal_VOID__INT,
					G_TYPE_NONE, 1, G_TYPE_UINT);

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (idle_muc_channel_class), &dbus_glib_idle_muc_channel_object_info);
}

void
idle_muc_channel_dispose (GObject *object)
{
  IdleMUCChannel *self = IDLE_MUC_CHANNEL (object);
  IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (!priv->closed)
  {
	  g_signal_emit(self, signals[CLOSED], 0);
	  priv->closed = TRUE;
  }

  if (G_OBJECT_CLASS (idle_muc_channel_parent_class)->dispose)
    G_OBJECT_CLASS (idle_muc_channel_parent_class)->dispose (object);
}
#if 0
/* unref all handles in given GArray */
static void unref_handle_array(IdleHandleStorage *storage, GArray *handles, TpHandleType type)
{
	int i;
	
	g_assert(storage != NULL);
	g_assert(handles != NULL);

	for (i=0; i<handles->len; i++)
	{
		idle_handle_unref(storage, type, (IdleHandle)(g_array_index(handles, guint, i)));
	}
}
#endif

#if 0
static void handle_unref_foreach(IdleHandleSet *set, IdleHandle handle, gpointer userdata)
{
	IdleHandleStorage *storage = IDLE_HANDLE_STORAGE(userdata);

	idle_handle_unref(storage, TP_HANDLE_TYPE_CONTACT, handle);
}
#endif

void
idle_muc_channel_finalize (GObject *object)
{
  IdleMUCChannel *self = IDLE_MUC_CHANNEL (object);
  IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE (self);
  IdleHandleStorage *handles;
  IdleMUCPendingMessage *msg;

  handles = _idle_connection_get_handles(priv->connection);
  idle_handle_unref(handles, TP_HANDLE_TYPE_ROOM, priv->handle);

  idle_handle_unref(handles, TP_HANDLE_TYPE_CONTACT, priv->own_handle);

  if (priv->object_path)
  {
	  g_free(priv->object_path);
  }

  if (priv->mode_state.topic)
  {
	  g_free(priv->mode_state.topic);
  }

  if (priv->mode_state.key)
  {
	  g_free(priv->mode_state.key);
  }

  while ((msg = g_queue_pop_head(priv->pending_messages)) != NULL)
  {
	  _idle_muc_pending_free(msg);
  }

  g_queue_free(priv->pending_messages);
  
  muc_channel_tp_properties_destroy(self);
  g_free(priv->properties);

  idle_handle_set_destroy(priv->current_members);
  idle_handle_set_destroy(priv->local_pending);
  idle_handle_set_destroy(priv->remote_pending);

  G_OBJECT_CLASS (idle_muc_channel_parent_class)->finalize (object);
}

static void muc_channel_tp_properties_init(IdleMUCChannel *chan)
{
	IdleMUCChannelPrivate *priv;
	TPProperty *props;
	int i;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);
	props = priv->properties;

	for (i=0; i<LAST_TP_PROPERTY_ENUM; i++)
	{
		GValue *value;
		props[i].value = value = g_new0(GValue, 1);

		g_value_init(value, property_signatures[i].type);
		
		props[i].flags = 0;
	}
}

static void muc_channel_tp_properties_destroy(IdleMUCChannel *chan)
{
	IdleMUCChannelPrivate *priv;
	TPProperty *props;
	int i;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);
	props = priv->properties;

	for (i=0; i<LAST_TP_PROPERTY_ENUM; i++)
	{
		g_value_unset(props[i].value);
		g_free(props[i].value);
	}
}

static gboolean g_value_compare(const GValue *v1, const GValue *v2)
{
	GType t1, t2;

	g_assert(v1 != NULL);
	g_assert(v2 != NULL);

	g_assert(G_IS_VALUE(v1));
	g_assert(G_IS_VALUE(v2));

	t1 = G_VALUE_TYPE(v1);
	t2 = G_VALUE_TYPE(v2);

	if (t1 != t2)
	{
		g_debug("%s: different types %s and %s compared!", G_STRFUNC, g_type_name(t1), g_type_name(t2));
		return FALSE;
	}

	switch (t1)
	{
		case G_TYPE_BOOLEAN:
			return g_value_get_boolean(v1) == g_value_get_boolean(v2);
		case G_TYPE_UINT:
			return g_value_get_uint(v1) == g_value_get_uint(v2);
		case G_TYPE_STRING:
		{
			const gchar *s1, *s2;

			s1 = g_value_get_string(v1);
			s2 = g_value_get_string(v2);

			if ((s1 == NULL) && (s2 == NULL))
			{
				return TRUE;
			}
			else if ((s1 == NULL) || (s2 == NULL))
			{
				return FALSE;
			}
			
			return (strcmp(s1, s2) == 0);
		}
		default:
			g_debug("%s: unknown type %s in comparison", G_STRFUNC, g_type_name(t1));
			return FALSE;
	}
}

static void change_tp_properties(IdleMUCChannel *chan, const GPtrArray *props)
{
	IdleMUCChannelPrivate *priv;
	int i;
	GPtrArray *changed_props;
	GArray *flags;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));
	g_assert(props != NULL);

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	changed_props = g_ptr_array_new();
	flags = g_array_new(FALSE, FALSE, sizeof(guint));

	for (i=0; i<props->len; i++)
	{
		GValue *curr_val;
		GValue prop = {0, };
		GValue *new_val;
		guint prop_id;

		g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_set_static_boxed(&prop, g_ptr_array_index(props, i));

		dbus_g_type_struct_get(&prop,
								0, &prop_id,
								1, &new_val,
								G_MAXUINT);

		if (prop_id >= LAST_TP_PROPERTY_ENUM)
		{
			g_debug("%s: prop_id >= LAST_TP_PROPERTY_ENUM, corruption!11", G_STRFUNC);
			continue;
		}
		
		curr_val = priv->properties[prop_id].value;

		if (!g_value_compare(new_val, curr_val))
		{
			g_value_copy(new_val, curr_val);

			g_ptr_array_add(changed_props, g_value_get_boxed(&prop));
			g_array_append_val(flags, prop_id);

			g_debug("%s: tp_property %u changed", G_STRFUNC, prop_id);
		}

		g_value_unset(&prop);
	}

	if (changed_props->len > 0)
	{
		g_debug("%s: emitting PROPERTIES_CHANGED with %u properties", G_STRFUNC, changed_props->len);
		g_signal_emit(chan, signals[PROPERTIES_CHANGED], 0, changed_props);
	}

	if (flags->len > 0)
	{
		g_debug("%s: flagging properties as readable with %u props", G_STRFUNC, flags->len);
		set_tp_property_flags(chan, flags, TP_PROPERTY_FLAG_READ, 0);
	}

	g_ptr_array_free(changed_props, TRUE);
	g_array_free(flags, TRUE);
}

static void set_tp_property_flags(IdleMUCChannel *chan, const GArray *props, TpPropertyFlags add, TpPropertyFlags remove)
{
	IdleMUCChannelPrivate *priv;
	int i;
	GPtrArray *changed_props;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	changed_props = g_ptr_array_new();

	if (props == NULL)
	{
		g_debug("%s: setting all flags with %u, %u", G_STRFUNC, add, remove);

		for (i=0; i<LAST_TP_PROPERTY_ENUM; i++)
		{
			guint curr_flags = priv->properties[i].flags;
			guint flags = (curr_flags | add) & (~remove);

			if (curr_flags != flags)
			{
				GValue prop = {0, };

				g_value_init(&prop, TP_TYPE_PROPERTY_FLAGS_STRUCT);
				g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_FLAGS_STRUCT));
				
				dbus_g_type_struct_set(&prop,
										0, i,
										1, flags,
										G_MAXUINT);

				priv->properties[i].flags = flags;

				g_ptr_array_add(changed_props, g_value_get_boxed(&prop));
			}
		}
	}
	else
	{
		for (i=0; i<props->len; i++)
		{
			guint prop_id = g_array_index(props, guint, i);
			guint curr_flags = priv->properties[prop_id].flags;
			guint flags = (curr_flags | add) & (~remove);

			if (curr_flags != flags)
			{
				GValue prop = {0, };

				g_value_init(&prop, TP_TYPE_PROPERTY_FLAGS_STRUCT);
				g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_FLAGS_STRUCT));
				
				dbus_g_type_struct_set(&prop,
										0, prop_id,
										1, flags,
										G_MAXUINT);

				priv->properties[prop_id].flags = flags;
				
				g_ptr_array_add(changed_props, g_value_get_boxed(&prop));
			}
		}
	}
	
	if (changed_props->len > 0)
	{
		g_debug("%s: emitting PROPERTY_FLAGS_CHANGED with %u properties", G_STRFUNC, changed_props->len);
		g_signal_emit(chan, signals[PROPERTY_FLAGS_CHANGED], 0, changed_props);
	}

	g_ptr_array_free(changed_props, TRUE);
}

static void change_sets(IdleMUCChannel *obj, TpIntSet *add_current, TpIntSet *remove_current, TpIntSet *add_local, TpIntSet *remove_local, TpIntSet *add_remote, TpIntSet *remove_remote, IdleHandle actor, TpChannelGroupChangeReason reason);

static void provide_password_reply(IdleMUCChannel *chan, gboolean success);

static void provide_password_reply(IdleMUCChannel *chan, gboolean success)
{
	IdleMUCChannelPrivate *priv;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	if (priv->passwd_ctx != NULL)
	{
		dbus_g_method_return(priv->passwd_ctx, success);
		priv->passwd_ctx = NULL;
	}
	else
	{
		g_debug("%s: don't have a ProvidePassword context to return with! (channel handle %u)", G_STRFUNC, priv->handle);
	}

	if (success)
	{
		change_password_flags(chan, TP_CHANNEL_PASSWORD_FLAG_PROVIDE, 0);
	}
}

static void change_state(IdleMUCChannel *obj, IdleMUCState state)
{
	IdleMUCChannelPrivate *priv;
	
	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	if ((state > MUC_STATE_JOINING) && (!priv->join_ready))
	{
		g_signal_emit(obj, signals[JOIN_READY], 0, MUC_CHANNEL_JOIN_ERROR_NONE);
		priv->join_ready = TRUE;
	}

	if (priv->state == MUC_STATE_NEED_PASSWORD && state == MUC_STATE_JOINED)
	{
		change_password_flags(obj, TP_CHANNEL_PASSWORD_FLAG_PROVIDE, FALSE);
		provide_password_reply(obj, TRUE);
	}
	
	if (priv->state == MUC_STATE_NEED_PASSWORD && state == MUC_STATE_NEED_PASSWORD)
	{
		provide_password_reply(obj, FALSE);
	}
	
	if (priv->state < MUC_STATE_NEED_PASSWORD && state == MUC_STATE_NEED_PASSWORD)
	{
		change_password_flags(obj, TP_CHANNEL_PASSWORD_FLAG_PROVIDE, TRUE);
	}

	priv->state = state;

	g_debug("%s: IdleMUCChannel %u changed to state %s", G_STRFUNC, priv->handle, ascii_muc_states[state]);
}

static void change_group_flags(IdleMUCChannel *obj, guint add, guint remove)
{
	IdleMUCChannelPrivate *priv;
	guint _add = 0, _remove = 0;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	_add = (~priv->group_flags) & add;
	_remove = priv->group_flags & remove;

	priv->group_flags |= add;
	priv->group_flags &= ~remove;

	if (_add | _remove)
	{
		g_signal_emit(obj, signals[GROUP_FLAGS_CHANGED], 0, add, remove);
		g_debug("%s: emitting GROUP_FLAGS_CHANGED with %u %u", G_STRFUNC, add, remove);
	}
}

static IdleMUCChannelTPProperty to_prop_id(IRCChannelModeFlags flag)
{
	switch (flag)
	{
		case MODE_FLAG_INVITE_ONLY:
			return TP_PROPERTY_INVITE_ONLY;
		case MODE_FLAG_MODERATED:
			return TP_PROPERTY_MODERATED;
		case MODE_FLAG_PRIVATE:
		case MODE_FLAG_SECRET:
			return TP_PROPERTY_PRIVATE;
		case MODE_FLAG_KEY:
			return TP_PROPERTY_PASSWORD_REQUIRED;
		case MODE_FLAG_USER_LIMIT:
			return TP_PROPERTY_LIMITED;
		default:
			return LAST_TP_PROPERTY_ENUM;
	}
}

static void change_mode_state(IdleMUCChannel *obj, guint add, guint remove)
{
	IdleMUCChannelPrivate *priv;
	IRCChannelModeFlags flags;
	guint group_add = 0, group_remove = 0;
	GPtrArray *tp_props_to_change;
	guint prop_flags = 0;
	guint combined;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	remove &= ~add;

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	flags = priv->mode_state.flags;

	tp_props_to_change = g_ptr_array_new();

	g_debug("%s: got %x, %x", G_STRFUNC, add, remove);

	add &= ~flags;
	remove &= flags;
	
	g_debug("%s: operation %x, %x", G_STRFUNC, add, remove);

	flags |= add;
	flags &= ~remove;

	combined = add|remove;

	if (add & MODE_FLAG_INVITE_ONLY)
	{
		if (!(flags & (MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE)))
		{
			group_remove |= TP_CHANNEL_GROUP_FLAG_CAN_ADD;
		}
	}
	else if (remove & MODE_FLAG_INVITE_ONLY)
	{
		group_add |= TP_CHANNEL_GROUP_FLAG_CAN_ADD;
	}

	if (combined & (MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE))
	{
		GArray *flags_to_change;
		
		static const guint flags_helper[] = 
		{
			TP_PROPERTY_INVITE_ONLY,
			TP_PROPERTY_LIMIT,
			TP_PROPERTY_LIMITED,
			TP_PROPERTY_MODERATED,
			TP_PROPERTY_PASSWORD,
			TP_PROPERTY_PASSWORD_REQUIRED,
			TP_PROPERTY_PRIVATE,
			TP_PROPERTY_SUBJECT,
			LAST_TP_PROPERTY_ENUM
		};

		flags_to_change = g_array_new(FALSE, FALSE, sizeof(guint));

		for (i=0; flags_helper[i] != LAST_TP_PROPERTY_ENUM; i++)
		{
			guint prop_id = flags_helper[i];
			g_array_append_val(flags_to_change, prop_id);
		}

		prop_flags = TP_PROPERTY_FLAG_WRITE;
		
		if (add & (MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE))
		{
			group_add |= TP_CHANNEL_GROUP_FLAG_CAN_ADD|TP_CHANNEL_GROUP_FLAG_CAN_REMOVE|TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE;
		
			set_tp_property_flags(obj, flags_to_change, prop_flags, 0);
		}
		else if (remove & (MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE))
		{
			group_remove |= TP_CHANNEL_GROUP_FLAG_CAN_REMOVE|TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE;

			if (flags & MODE_FLAG_INVITE_ONLY)
			{
				group_remove |= TP_CHANNEL_GROUP_FLAG_CAN_ADD;
			}
		
			set_tp_property_flags(obj, flags_to_change, 0, prop_flags);
		}
	}

	for (i = 1; i<LAST_MODE_FLAG_ENUM; i = (i << 1))
	{
		if (combined & i)
		{
			IdleMUCChannelTPProperty tp_prop_id;		
			
			tp_prop_id = to_prop_id(i);

			if (tp_prop_id < LAST_TP_PROPERTY_ENUM)
			{
				GValue prop = {0, };
				GValue val_auto_is_fine = {0, };
				GValue *val = &val_auto_is_fine;
				GType type = property_signatures[tp_prop_id].type;
	
				g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
				g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));

				g_value_init(val, type);

				if (type != G_TYPE_BOOLEAN)
				{
					g_debug("%s: type != G_TYPE_BOOLEAN for %u (modeflag %u), ignoring", G_STRFUNC, tp_prop_id, i);
					continue;
				}

				g_value_set_boolean(val, (add & i) ? TRUE : FALSE);

				dbus_g_type_struct_set(&prop,
										0, tp_prop_id,
										1, val,
										G_MAXUINT);

				g_ptr_array_add(tp_props_to_change, g_value_get_boxed(&prop));

				if (add & i)
				{
					GValue prop = {0, };
					GValue val_auto_is_fine = {0, };
					GValue *val = &val_auto_is_fine;

					g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
					g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));

					if (i == MODE_FLAG_USER_LIMIT)
					{
						g_value_init(val, G_TYPE_UINT);
						g_value_set_uint(val, priv->mode_state.limit);
						tp_prop_id = TP_PROPERTY_LIMIT;
					}
					else if (i == MODE_FLAG_KEY)
					{
						g_value_init(val, G_TYPE_STRING);
						g_value_set_string(val, priv->mode_state.key);
						tp_prop_id = TP_PROPERTY_PASSWORD;
					}
					else
					{
						continue;
					}

					dbus_g_type_struct_set(&prop,
											0, tp_prop_id,
											1, val,
											G_MAXUINT);

					g_ptr_array_add(tp_props_to_change, g_value_get_boxed(&prop));
				}
			}
		}
	}

	change_group_flags(obj, group_add, group_remove);
	change_tp_properties(obj, tp_props_to_change);

	priv->mode_state.flags = flags;

	g_debug("%s: changed to %x", G_STRFUNC, flags);
}

static void change_password_flags(IdleMUCChannel *obj, guint flag, gboolean state)
{
	IdleMUCChannelPrivate *priv;
	guint add = 0, remove = 0;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	if (state)
	{
		add = (~(priv->password_flags)) & flag;
		priv->password_flags |= flag;
	}
	else
	{
		remove = priv->password_flags & flag;
		priv->password_flags &= ~flag;
	}

	if (add | remove)
	{
		g_debug("%s: emitting PASSWORD_FLAGS_CHANGED with %u %u", G_STRFUNC, add, remove);
		g_signal_emit(obj, signals[PASSWORD_FLAGS_CHANGED], 0, add, remove);
	}
}

static void change_sets(IdleMUCChannel *obj,
						TpIntSet *add_current,
						TpIntSet *remove_current,
						TpIntSet *add_local,
						TpIntSet *remove_local,
						TpIntSet *add_remote,
						TpIntSet *remove_remote,
						IdleHandle actor,
						TpChannelGroupChangeReason reason)
{
	IdleMUCChannelPrivate *priv;
	TpIntSet *add, *remove, *local_pending, *remote_pending, *tmp1, *tmp2;
	GArray *vadd, *vremove, *vlocal, *vremote;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	add = tp_intset_new();
	remove = tp_intset_new();
	local_pending = tp_intset_new();
	remote_pending = tp_intset_new();

	if (add_current)
	{
		tmp1 = idle_handle_set_update(priv->current_members, add_current);
		tmp2 = tp_intset_union(add, tmp1);

		tp_intset_destroy(add);
		add = tmp2;
		
		tp_intset_destroy(tmp1);
	}

	if (remove_current)
	{
		tmp1 = idle_handle_set_difference_update(priv->current_members, remove_current);
		tmp2 = tp_intset_union(remove, tmp1);
		
		tp_intset_destroy(remove);
		remove = tmp2;

		tp_intset_destroy(tmp1);
	}
	
	if (add_local)
	{
		tmp1 = idle_handle_set_update(priv->local_pending, add_local);
		tmp2 = tp_intset_union(local_pending, tmp1);

		tp_intset_destroy(local_pending);
		local_pending = tmp2;

		tp_intset_destroy(tmp1);
	}

	if (remove_local)
	{
		tmp1 = idle_handle_set_difference_update(priv->local_pending, remove_local);
		tmp2 = tp_intset_union(remove, tmp1);

		tp_intset_destroy(remove);
		remove = tmp2;

		tp_intset_destroy(tmp1);
	}

	if (add_remote)
	{
		tmp1 = idle_handle_set_update(priv->remote_pending, add_remote);
		tmp2 = tp_intset_union(remote_pending, tmp1);

		tp_intset_destroy(remote_pending);
		remote_pending = tmp2;

		tp_intset_destroy(tmp1);
	}

	if (remove_remote)
	{
		tmp1 = idle_handle_set_difference_update(priv->remote_pending, remove_remote);
		tmp2 = tp_intset_union(remove, tmp1);

		tp_intset_destroy(remove);
		remove = tmp2;

		tp_intset_destroy(tmp1);
	}
	
	tmp1 = tp_intset_difference(remove, add);
	tp_intset_destroy(remove);
	remove = tmp1;

	tmp1 = tp_intset_difference(remove, local_pending);
	tp_intset_destroy(remove);
	remove = tmp1;

	tmp1 = tp_intset_difference(remove, remote_pending);
	tp_intset_destroy(remove);
	remove = tmp1;

	vadd = tp_intset_to_array(add);
	vremove = tp_intset_to_array(remove);
	vlocal = tp_intset_to_array(local_pending);
	vremote = tp_intset_to_array(remote_pending);

	if ((vadd->len + vremove->len + vlocal->len + vremote->len) > 0)
	{
		g_debug("%s: emitting MEMBERS_CHANGED for channel with handle %u, amounts to (%u, %u, %u, %u)", G_STRFUNC, priv->handle, vadd->len, vremove->len, vlocal->len, vremote->len);
		g_signal_emit(obj, signals[MEMBERS_CHANGED], 0, "", vadd, vremove, vlocal, vremote, actor, reason);
	}

	g_array_free(vadd, TRUE);
	g_array_free(vremove, TRUE);
	g_array_free(vlocal, TRUE);
	g_array_free(vremote, TRUE);

	tp_intset_destroy(add);
	tp_intset_destroy(remove);
	tp_intset_destroy(local_pending);
	tp_intset_destroy(remote_pending);
}

gboolean _idle_muc_channel_receive(IdleMUCChannel *chan, TpChannelTextMessageType type, IdleHandle sender, const gchar *text)
{
	IdleMUCChannelPrivate *priv;
	IdleMUCPendingMessage *msg;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);
	
	msg = _idle_muc_pending_new();

	msg->id = priv->recv_id++;
	msg->timestamp = time(NULL);
	msg->sender = sender;
	msg->type = type;
	msg->text = g_strdup(text);

	g_queue_push_tail(priv->pending_messages, msg);

	g_signal_emit(chan, signals[RECEIVED], 0,
						msg->id,
						msg->timestamp,
						msg->sender,
						msg->type,
						0,
						msg->text);

	g_debug("%s: queued message %u", G_STRFUNC, msg->id);
	
	return FALSE;
}

static void send_mode_query_request(IdleMUCChannel *chan)
{
	IdleMUCChannelPrivate *priv;
	gchar cmd[IRC_MSG_MAXLEN+2];

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	g_snprintf(cmd, IRC_MSG_MAXLEN+2, "MODE %s", priv->channel_name);

	_idle_connection_send(priv->connection, cmd);
}

void _idle_muc_channel_join(IdleMUCChannel *chan, const gchar *nick)
{
	IdleMUCChannelPrivate *priv;
	IdleHandle handle;
	TpIntSet *set;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));
	g_assert(nick != NULL);

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	set = tp_intset_new();

	handle = idle_handle_for_contact(_idle_connection_get_handles(priv->connection), nick);

	if (handle == 0)
	{
		g_debug("%s: invalid nick (%s)", G_STRFUNC, nick);
		
		return;
	}

	if (handle == priv->own_handle)
	{
		/* woot we managed to get into a channel, great */
		change_state(chan, MUC_STATE_JOINED);
		tp_intset_add(set, handle);
		change_sets(chan, set, NULL, NULL, set, NULL, set, handle, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
		change_group_flags(chan, TP_CHANNEL_GROUP_FLAG_CAN_ADD, 0);

		send_mode_query_request(chan);
	
		if (priv->channel_name[0] == '+')
		{
			/* according to IRC specs, PLUS channels do not support channel modes and alway have only +t set, so we work with that. */
			change_mode_state(chan, MODE_FLAG_TOPIC_ONLY_SETTABLE_BY_OPS, 0);
		}
	}
	else
	{
		tp_intset_add(set, handle);

		change_sets(chan, set, NULL, NULL, NULL, NULL, set, handle, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
	}

	g_debug("%s: member joined with handle %u and nick %s", G_STRFUNC, handle, nick);

	tp_intset_destroy(set);
}

void _idle_muc_channel_part(IdleMUCChannel *chan, const gchar *nick)
{
	return _idle_muc_channel_kick(chan, nick, nick, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
}

void _idle_muc_channel_kick(IdleMUCChannel *chan, const gchar *nick, const gchar *kicker, TpChannelGroupChangeReason reason)
{
	IdleMUCChannelPrivate *priv;
	IdleHandle handle;
	IdleHandle kicker_handle;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	handle = idle_handle_for_contact(_idle_connection_get_handles(priv->connection), nick);

	if (handle == 0)
	{
		g_debug("%s: failed to get handle for (%s)", G_STRFUNC, nick);

		return;
	}

	kicker_handle = idle_handle_for_contact(_idle_connection_get_handles(priv->connection), kicker);

	if (kicker_handle == 0)
	{
		g_debug("%s: failed to get handle for (%s)", G_STRFUNC, kicker);
	}
	
	return _idle_muc_channel_handle_quit(chan,
										 handle,
										 FALSE,
										 kicker_handle,
										 reason);
}

void _idle_muc_channel_handle_quit(IdleMUCChannel *chan,
								   IdleHandle handle,
								   gboolean suppress,
								   IdleHandle actor,
								   TpChannelGroupChangeReason reason)
{
	IdleMUCChannelPrivate *priv;
	TpIntSet *set;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	set = tp_intset_new();

	tp_intset_add(set, handle);

	change_sets(chan, NULL, set, NULL, set, NULL, set, actor, reason);
	
	if (handle == priv->own_handle)
	{
		g_debug("%s: it was us!", G_STRFUNC);
		
		change_state(chan, MUC_STATE_PARTED);

		if (!suppress)
		{
			priv->closed = TRUE;

			g_signal_emit(chan, signals[CLOSED], 0);
		}
	}

	tp_intset_destroy(set);
}

void _idle_muc_channel_invited(IdleMUCChannel *chan, IdleHandle inviter)
{
	IdleMUCChannelPrivate *priv;
	TpIntSet *handles_to_add;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	handles_to_add = tp_intset_new();

	tp_intset_add(handles_to_add, priv->own_handle);

	change_sets(chan, NULL, NULL, handles_to_add, NULL, NULL, NULL, inviter, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);

	tp_intset_destroy(handles_to_add);
}

void _idle_muc_channel_names(IdleMUCChannel *chan, GArray *names)
{
	IdleMUCChannelPrivate *priv;
	int i;
	TpIntSet *handles_to_add;
	IdleHandleStorage *handles; 

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));
	g_assert(names != NULL);

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	handles = _idle_connection_get_handles(priv->connection);
	
	handles_to_add = tp_intset_new();

	for (i=0; i<names->len; i++)
	{
		IdleHandle handle;
		gchar *nick = g_array_index(names, gchar *, i);
		gunichar ucs4char;

		gchar own_mode = '\0';

		ucs4char = g_utf8_get_char_validated(nick, -1);
		
		if (!g_unichar_isalpha(ucs4char))
		{
			own_mode = *nick;
			
			nick++;
		}

		handle = idle_handle_for_contact(handles, nick);

		if (handle == 0)
		{
			g_debug("%s: failed to get valid handle for nick %s, ignoring", G_STRFUNC, nick);
			continue;
		}

		if (handle == priv->own_handle)
		{
			guint remove = MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_VOICE_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE;
			guint add = 0;
			
			switch (own_mode)
			{
				case '@':
				{
					g_debug("%s: we are OP", G_STRFUNC);
					add |= MODE_FLAG_OPERATOR_PRIVILEGE;
				}
				break;
				case '&':
				{
					g_debug("%s: we are HALFOP", G_STRFUNC);
					add |= MODE_FLAG_HALFOP_PRIVILEGE;
				}
				break;
				case '+':
				{
					g_debug("%s: we are VOICED", G_STRFUNC);
					add |= MODE_FLAG_VOICE_PRIVILEGE;
				}
				break;
				default:
				{
					g_debug("%s: we are NORMAL", G_STRFUNC);
				}
				break;
			}

			remove &= ~add;

			change_mode_state(chan, add, remove);
		}

		tp_intset_add(handles_to_add, handle);

	}

	change_sets(chan, handles_to_add, NULL, NULL, handles_to_add, NULL, handles_to_add, 0, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
}

void _idle_muc_channel_mode(IdleMUCChannel *chan, const gchar *mode_str)
{
	IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);
	gchar **mode_argv;
	gchar *operation;
	gboolean remove;
	guint mode_accum = 0;
	guint limit = 0;
	gchar *key = NULL;
	const gchar *own_nick;
	GArray *flags_to_change;
	static const guint flags_helper[] = 
	{
		TP_PROPERTY_INVITE_ONLY,
		TP_PROPERTY_LIMITED,
		TP_PROPERTY_MODERATED,
		TP_PROPERTY_PASSWORD_REQUIRED,
		TP_PROPERTY_PRIVATE,
		LAST_TP_PROPERTY_ENUM
	};

	int i = 1;

	mode_argv = g_strsplit_set(mode_str, " ", -1);

	if (mode_argv[0] == NULL)
	{
		g_debug("%s: failed to parse (%s) to tokens", G_STRFUNC, mode_str);
		goto cleanupl;
	}

	operation = mode_argv[0]+1;

	if (mode_argv[0][0] == '+')
	{
		remove = FALSE;
	}
	else if (mode_argv[0][0] == '-')
	{
		remove = TRUE;
	}
	else
	{
		g_debug("%s: failed to decide whether to add or remove modes in (%s)", G_STRFUNC, mode_argv[0]);
		goto cleanupl;
	}

	own_nick = idle_handle_inspect(_idle_connection_get_handles(priv->connection), TP_HANDLE_TYPE_CONTACT, priv->own_handle);
	
	while (*operation != '\0')
	{
		switch (*operation)
		{
			case 'o':
			{
				if (g_strncasecmp(own_nick, mode_argv[i++], -1) == 0)
				{
					g_debug("%s: got MODE o concerning us", G_STRFUNC);
					mode_accum |= MODE_FLAG_OPERATOR_PRIVILEGE;
				}
			}
			break;
			case 'h':
			{
				if (g_strncasecmp(own_nick, mode_argv[i++], -1) == 0)
				{
					g_debug("%s: got MODE h concerning us", G_STRFUNC);
					mode_accum |= MODE_FLAG_HALFOP_PRIVILEGE;
				}
			}
			break;
			case 'v':
			{
				if (g_strncasecmp(own_nick, mode_argv[i++], -1) == 0)
				{
					g_debug("%s: got MODE v concerning us", G_STRFUNC);
					mode_accum |= MODE_FLAG_VOICE_PRIVILEGE;
				}
			}
			break;
			case 'l':
			{
				limit = atoi(mode_argv[i++]);
				g_debug("%s: got channel user limit %u", G_STRFUNC, limit);
				mode_accum |= MODE_FLAG_USER_LIMIT;
			}
			break;
			case 'k':
			{
				key = g_strdup(mode_argv[i++]);
				g_debug("%s: got channel key %s", G_STRFUNC, key);
				mode_accum |= MODE_FLAG_KEY;
			}
			break;
			case 'a':
			{
				mode_accum |= MODE_FLAG_ANONYMOUS;
			}
			break;
			case 'i':
			{
				mode_accum |= MODE_FLAG_INVITE_ONLY;
			}
			break;
			case 'm':
			{
				mode_accum |= MODE_FLAG_MODERATED;
			}
			break;
			case 'n':
			{
				mode_accum |= MODE_FLAG_NO_OUTSIDE_MESSAGES;
			}
			break;
			case 'q':
			{
				mode_accum |= MODE_FLAG_QUIET;
			}
			break;
			case 'p':
			{
				mode_accum |= MODE_FLAG_PRIVATE;
			}
			break;
			case 's':
			{
				mode_accum |= MODE_FLAG_SECRET;
			}
			break;
			case 'r':
			{
				mode_accum |= MODE_FLAG_SERVER_REOP;
			}
			break;
			case 't':
			{
				mode_accum |= MODE_FLAG_TOPIC_ONLY_SETTABLE_BY_OPS;
			}
			break;
			default:
			{
				g_debug("%s: did not understand mode identifier %c", G_STRFUNC, *operation);
			}
			break;
		}
		operation++;
	}

	if (mode_accum & MODE_FLAG_KEY)
	{
		priv->mode_state.key = key;
	}
	if (mode_accum & MODE_FLAG_USER_LIMIT)
	{
		priv->mode_state.limit = limit;
	}
	
	flags_to_change = g_array_new(FALSE, FALSE, sizeof(guint));

	for (i=0; flags_helper[i] != LAST_TP_PROPERTY_ENUM; i++)
	{
		guint prop_id = flags_helper[i];
		g_array_append_val(flags_to_change, prop_id);
	}

	set_tp_property_flags(chan, flags_to_change, TP_PROPERTY_FLAG_READ, 0);

	if (!remove)
	{
		change_mode_state(chan, mode_accum, 0);
	}
	else
	{
		change_mode_state(chan, 0, mode_accum);
	}

cleanupl:

	g_strfreev(mode_argv);
}

void _idle_muc_channel_topic(IdleMUCChannel *chan, const char *topic)
{
	GValue prop = {0, };
	GValue val = {0, };
	GPtrArray *arr;
	IdleMUCChannelPrivate *priv;
	
	g_assert(chan != NULL);
	g_assert(topic != NULL);

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
	g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));

	g_value_init(&val, G_TYPE_STRING);
	g_value_set_string(&val, topic);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT,
							1, &val,
							G_MAXUINT);
	
	arr = g_ptr_array_new();

	g_ptr_array_add(arr, g_value_get_boxed(&prop));

	change_tp_properties(chan, arr);
	
	g_ptr_array_free(arr, TRUE);
}

void _idle_muc_channel_topic_touch(IdleMUCChannel *chan, const IdleHandle toucher, const guint timestamp)
{
	GValue prop = {0, };
	GValue val = {0, };
	GPtrArray *arr;
	
	arr = g_ptr_array_new();
	
	g_assert(chan != NULL);
	g_assert(toucher != 0);

	g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
	g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));
		
	g_value_init(&val, G_TYPE_UINT);
	g_value_set_uint(&val, toucher);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT_CONTACT,
							1, &val,
							G_MAXUINT);
	
	g_ptr_array_add(arr, g_value_get_boxed(&prop));

	g_value_set_uint(&val, timestamp);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT_TIMESTAMP,
							1, &val,
							G_MAXUINT);
	
	g_ptr_array_add(arr, g_value_get_boxed(&prop));

	change_tp_properties(chan, arr);
	
	g_ptr_array_free(arr, TRUE);
}

void _idle_muc_channel_topic_full(IdleMUCChannel *chan, const IdleHandle toucher, const guint timestamp, const gchar *topic)
{
	GValue prop = {0, };
	GValue val = {0, };
	GPtrArray *arr;
	
	arr = g_ptr_array_new();
	
	g_assert(chan != NULL);
	g_assert(toucher != 0);

	g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
	g_value_take_boxed(&prop, dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));
		
	g_value_init(&val, G_TYPE_UINT);
	g_value_set_uint(&val, toucher);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT_CONTACT,
							1, &val,
							G_MAXUINT);
	
	g_ptr_array_add(arr, g_value_get_boxed(&prop));

	g_value_set_uint(&val, timestamp);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT_TIMESTAMP,
							1, &val,
							G_MAXUINT);
	
	g_ptr_array_add(arr, g_value_get_boxed(&prop));

	g_value_unset(&val);
	g_value_init(&val, G_TYPE_STRING);
	g_value_set_string(&val, topic);

	dbus_g_type_struct_set(&prop,
							0, TP_PROPERTY_SUBJECT,
							1, &val,
							G_MAXUINT);

	change_tp_properties(chan, arr);
	
	g_ptr_array_free(arr, TRUE);
}

void _idle_muc_channel_topic_unset(IdleMUCChannel *chan)
{
	GArray *arr = g_array_new(FALSE, FALSE, sizeof(guint));
	guint not_even_g_array_append_can_take_address_of_enumeration_constants_in_c;
	guint *tmp = &not_even_g_array_append_can_take_address_of_enumeration_constants_in_c;

	*tmp = TP_PROPERTY_SUBJECT;
	g_array_append_val(arr, *tmp);
	*tmp = TP_PROPERTY_SUBJECT_TIMESTAMP;
	g_array_append_val(arr, *tmp);
	*tmp = TP_PROPERTY_SUBJECT_CONTACT;
	g_array_append_val(arr, *tmp);

	set_tp_property_flags(chan, arr, 0, TP_PROPERTY_FLAG_READ);

	g_array_free(arr, TRUE);
}

void _idle_muc_channel_badchannelkey(IdleMUCChannel *chan)
{
	change_state(chan, MUC_STATE_NEED_PASSWORD);
}

void _idle_muc_channel_join_error(IdleMUCChannel *chan, IdleMUCChannelJoinError err)
{
	IdleMUCChannelPrivate *priv;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	if (!priv->join_ready)
	{
		priv->join_ready = TRUE;
		
		g_signal_emit(chan, signals[JOIN_READY], 0, err);
	}
	else
	{
		g_debug("%s: already emitted JOIN_READY! (current err %u)", G_STRFUNC, err);
	}
}

void _idle_muc_channel_rename(IdleMUCChannel *chan, IdleHandle old, IdleHandle new)
{
	IdleMUCChannelPrivate *priv;
	TpIntSet *cadd, *cremove, *ladd, *lremove, *radd, *rremove;

	g_assert(chan != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(chan));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	cadd = tp_intset_new();
	cremove = tp_intset_new();
	ladd = tp_intset_new();
	lremove = tp_intset_new();
	radd = tp_intset_new();
	rremove = tp_intset_new();

	if (priv->own_handle == old)
	{
		gboolean valid;
		IdleHandleStorage *handles;

		handles = _idle_connection_get_handles(priv->connection);
		
		valid = idle_handle_unref(handles, TP_HANDLE_TYPE_CONTACT, old);
		g_assert(valid);
		
		priv->own_handle = new;
		
		valid = idle_handle_ref(handles, TP_HANDLE_TYPE_CONTACT, new);
		g_assert(valid);

		g_debug("%s: changed own_handle to %u", G_STRFUNC, new);
	}
	
	if (idle_handle_set_contains(priv->current_members, old))
	{
		tp_intset_add(cadd, new);
		tp_intset_add(cremove, old);
	}
	else if (idle_handle_set_contains(priv->local_pending, old))
	{
		tp_intset_add(ladd, new);
		tp_intset_add(lremove, old);
	}
	else if (idle_handle_set_contains(priv->remote_pending, old))
	{
		tp_intset_add(radd, new);
		tp_intset_add(rremove, old);
	}

	change_sets(chan, cadd, cremove, ladd, lremove, radd, rremove, new, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);

	tp_intset_destroy(cadd);
	tp_intset_destroy(cremove);
	tp_intset_destroy(ladd);
	tp_intset_destroy(lremove);
	tp_intset_destroy(radd);
	tp_intset_destroy(rremove);
}

static void send_join_request(IdleMUCChannel *obj, const gchar *password)
{
	IdleMUCChannelPrivate *priv;
	gchar cmd[IRC_MSG_MAXLEN+1];

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	
	if (password)
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "JOIN %s %s", priv->channel_name, password);
	}
	else
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "JOIN %s", priv->channel_name);
	}

	_idle_connection_send(priv->connection, cmd);
}

void _idle_muc_channel_join_attempt(IdleMUCChannel *obj)
{
	return send_join_request(obj, NULL);
}

gboolean _idle_muc_channel_has_current_member(IdleMUCChannel *chan, IdleHandle handle)
{
	IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE(chan);

	return idle_handle_set_contains(priv->current_members, handle);
}

static gboolean send_invite_request(IdleMUCChannel *obj, IdleHandle handle, GError **error)
{
	IdleMUCChannelPrivate *priv;
	gchar cmd[IRC_MSG_MAXLEN+1];
	const gchar *nick;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	nick = idle_handle_inspect(_idle_connection_get_handles(priv->connection), TP_HANDLE_TYPE_CONTACT, handle);

	if ((nick == NULL) || (nick[0] == '\0'))
	{
		g_debug("%s: invalid handle %u passed", G_STRFUNC, handle);
		
		*error = g_error_new(TELEPATHY_ERRORS, InvalidHandle, "invalid handle %u passed", handle);

		return FALSE;
	}

	g_snprintf(cmd, IRC_MSG_MAXLEN+1, "INVITE %s %s", nick, priv->channel_name);

	_idle_connection_send(priv->connection, cmd);

	return TRUE;
}

static gboolean send_kick_request(IdleMUCChannel *obj, IdleHandle handle, const gchar *msg, GError **error)
{
	IdleMUCChannelPrivate *priv;
	gchar cmd[IRC_MSG_MAXLEN+1];
	const gchar *nick;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	nick = idle_handle_inspect(_idle_connection_get_handles(priv->connection), TP_HANDLE_TYPE_CONTACT, handle);

	if ((nick == NULL) || (nick[0] == '\0'))
	{
		g_debug("%s: invalid handle %u passed", G_STRFUNC, handle);
		
		*error = g_error_new(TELEPATHY_ERRORS, InvalidHandle, "invalid handle %u passed", handle);

		return FALSE;
	}

	if (msg != NULL)
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "KICK %s %s %s", priv->channel_name, nick, msg);
	}
	else
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "KICK %s %s", priv->channel_name, nick);
	}

	_idle_connection_send(priv->connection, cmd);

	return TRUE;
}

static gboolean add_member(IdleMUCChannel *obj, IdleHandle handle, GError **error)
{
	IdleMUCChannelPrivate *priv;
	IdleHandleStorage *handles;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	handles = _idle_connection_get_handles(priv->connection);
	
	if (handle == priv->own_handle)
	{
		if (idle_handle_set_contains(priv->current_members, handle) || idle_handle_set_contains(priv->remote_pending, handle))
		{
			g_debug("%s: we are already a member of or trying to join the channel with handle %u", G_STRFUNC, priv->handle);

			*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "we are already a member of or trying to join the channel with handle %u", priv->handle);

			return FALSE;
		}
		else
		{
			TpIntSet *add_set = tp_intset_new();
			
			send_join_request(obj, NULL);

			change_state(obj, MUC_STATE_JOINING);

			tp_intset_add(add_set, handle);

			change_sets(obj, NULL, NULL, NULL, NULL, add_set, NULL, 0, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
		}
	}
	else
	{
		if (idle_handle_set_contains(priv->current_members, handle) || idle_handle_set_contains(priv->remote_pending, handle))
		{
			g_debug("%s: the requested contact (handle %u) to be added to the room (handle %u) is already a member of or has already been invited to join the room", G_STRFUNC, handle, priv->handle);

			*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "the requested contact (handle %u) to be added to the room (handle %u) is already a member of or has already been invited to join the room", handle, priv->handle);

			return FALSE;
		}
		else
		{
			GError *invite_error;
			TpIntSet *add_set = tp_intset_new();
			
			if (!send_invite_request(obj, handle, &invite_error))
			{
				*error = invite_error;

				return FALSE;
			}

			tp_intset_add(add_set, handle);

			change_sets(obj,
						NULL, NULL,
						NULL, NULL,
						add_set, NULL,
						priv->own_handle,
						TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);
		}
	}

	return TRUE;
}

static gint idle_pending_message_compare(gconstpointer msg, gconstpointer id)
{
	IdleMUCPendingMessage *message = (IdleMUCPendingMessage *)(msg);

	return (message->id != GPOINTER_TO_INT(id));
}

/**
 * idle_muc_channel_acknowledge_pending_messages
 *
 * Implements DBus method AcknowledgePendingMessages
 * on interface org.freedesktop.Telepathy.Channel.Type.Text
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_acknowledge_pending_messages (IdleMUCChannel *obj,
														const GArray *ids,
														GError **error)
{
	IdleMUCChannelPrivate *priv;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	for (i=0; i<ids->len; i++)
	{
		GList *node;
		IdleMUCPendingMessage *msg;
		guint id = g_array_index(ids, guint, i);

		node = g_queue_find_custom(priv->pending_messages,
								   GINT_TO_POINTER(id),
								   idle_pending_message_compare);

		if (node == NULL)
		{
			g_debug("%s: message %u not found", G_STRFUNC, id);

			*error = g_error_new(TELEPATHY_ERRORS, InvalidArgument, "message id %u not found", id);

			return FALSE;
		}

		msg = (IdleMUCPendingMessage *)(node->data);

		g_debug("%s: acknowledging pending message with id %u", G_STRFUNC, id);

		g_queue_delete_link(priv->pending_messages, node);

		_idle_muc_pending_free(msg);
	}

	return TRUE;
}

/**
 * idle_muc_channel_add_members
 *
 * Implements DBus method AddMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_add_members (IdleMUCChannel *obj, const GArray * contacts, const gchar * message, GError **error)
{
	IdleMUCChannelPrivate *priv;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	for (i=0; i < contacts->len; i++)
	{
		IdleHandle handle;
		GError *add_error;

		handle = (IdleHandle)(g_array_index(contacts, guint, i));

		if (!add_member(obj, handle, &add_error))
		{
			*error = add_error;

			return FALSE;
		}
	}
	
	return TRUE;
}

static void part_from_channel(IdleMUCChannel *obj, const gchar *msg)
{
	IdleMUCChannelPrivate *priv;
	gchar cmd[IRC_MSG_MAXLEN+1];

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	if (msg != NULL)
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "PART %s %s", priv->channel_name, msg);
	}
	else
	{
		g_snprintf(cmd, IRC_MSG_MAXLEN+1, "PART %s", priv->channel_name);
	}

	_idle_connection_send(priv->connection, cmd);
}


/**
 * idle_muc_channel_close
 *
 * Implements DBus method Close
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_close (IdleMUCChannel *obj, GError **error)
{
	IdleMUCChannelPrivate *priv;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	
	if (priv->state == MUC_STATE_JOINED)
	{
		part_from_channel(obj, NULL);
	}

	if (priv->state < MUC_STATE_JOINED)
	{
		if (!priv->closed)
		{
			g_signal_emit(obj, signals[CLOSED], 0);
			priv->closed = TRUE;
		}
	}
	
	g_debug("%s: called on %p", G_STRFUNC, obj);

	return TRUE;
}

/**
 * idle_muc_channel_get_all_members
 *
 * Implements DBus method GetAllMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_all_members (IdleMUCChannel *obj, GArray ** ret, GArray ** ret1, GArray ** ret2, GError **error)
{
	IdleMUCChannelPrivate *priv;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = idle_handle_set_to_array(priv->current_members);
	*ret1 = idle_handle_set_to_array(priv->local_pending);
	*ret2 = idle_handle_set_to_array(priv->remote_pending);

	return TRUE;
}


/**
 * idle_muc_channel_get_channel_type
 *
 * Implements DBus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_channel_type (IdleMUCChannel *obj, gchar ** ret, GError **error)
{
	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	*ret = g_strdup(TP_IFACE_CHANNEL_TYPE_TEXT);
	
	return TRUE;
}


/**
 * idle_muc_channel_get_group_flags
 *
 * Implements DBus method GetGroupFlags
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_group_flags (IdleMUCChannel *obj, guint* ret, GError **error)
{
	IdleMUCChannelPrivate *priv;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));
	
	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	
	*ret = priv->group_flags;			
		
	return TRUE;
}


/**
 * idle_muc_channel_get_handle
 *
 * Implements DBus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_handle (IdleMUCChannel *obj, guint* ret, guint* ret1, GError **error)
{
	IdleMUCChannelPrivate *priv;
	
	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = TP_HANDLE_TYPE_ROOM;
	*ret1 = priv->handle;

	g_debug("%s: returning handle %u", G_STRFUNC, *ret1);
	
	return TRUE;
}


/**
 * idle_muc_channel_get_handle_owners
 *
 * Implements DBus method GetHandleOwners
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_handle_owners (IdleMUCChannel *obj, const GArray * handles, GArray ** ret, GError **error)
{
	int i;
	
	*ret = g_array_sized_new(FALSE, FALSE, sizeof(guint), handles->len);

	for (i=0; i<handles->len; i++)
	{
		g_array_index(*ret, guint, i) = g_array_index(handles, guint, i);
	}
	
	return TRUE;
}


/**
 * idle_muc_channel_get_interfaces
 *
 * Implements DBus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_interfaces (IdleMUCChannel *obj, gchar *** ret, GError **error)
{
	const gchar *interfaces[] = {TP_IFACE_CHANNEL_INTERFACE_PASSWORD, TP_IFACE_CHANNEL_INTERFACE_GROUP, TP_IFACE_PROPERTIES_INTERFACE, NULL};
	
	*ret = g_strdupv((gchar **)(interfaces));

	return TRUE;
}


/**
 * idle_muc_channel_get_local_pending_members
 *
 * Implements DBus method GetLocalPendingMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_local_pending_members (IdleMUCChannel *obj, GArray ** ret, GError **error)
{
	IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);
	
	*ret = idle_handle_set_to_array(priv->local_pending);

  return TRUE;
}


/**
 * idle_muc_channel_get_members
 *
 * Implements DBus method GetMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_members (IdleMUCChannel *obj, GArray ** ret, GError **error)
{
	IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = idle_handle_set_to_array(priv->current_members);
	
  return TRUE;
}


/**
 * idle_muc_channel_get_message_types
 *
 * Implements DBus method GetMessageTypes
 * on interface org.freedesktop.Telepathy.Channel.Type.Text
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_message_types (IdleMUCChannel *obj, GArray ** ret, GError **error)
{
	int i;
	
	*ret = g_array_sized_new(FALSE, FALSE, sizeof(guint), 3);

	for (i=0; i<3; i++)
	{
		g_array_index(*ret, guint, i) = i;
	}
	
  return TRUE;
}


/**
 * idle_muc_channel_get_password_flags
 *
 * Implements DBus method GetPasswordFlags
 * on interface org.freedesktop.Telepathy.Channel.Interface.Password
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_password_flags (IdleMUCChannel *obj, guint* ret, GError **error)
{
	IdleMUCChannelPrivate *priv;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = priv->password_flags;
	
  	return TRUE;
}


/**
 * idle_muc_channel_get_properties
 *
 * Implements DBus method GetProperties
 * on interface org.freedesktop.Telepathy.Properties
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_properties (IdleMUCChannel *obj, const GArray * properties, GPtrArray ** ret, GError **error)
{
	IdleMUCChannelPrivate *priv;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));
	
	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	for (i=0; i<properties->len; i++)
	{
		IdleMUCChannelTPProperty prop = g_array_index(properties, guint, i);

		if (prop >= LAST_TP_PROPERTY_ENUM)
		{
			g_debug("%s: invalid property id %u", G_STRFUNC, prop);

			*error = g_error_new(TELEPATHY_ERRORS, InvalidArgument, "invalid property id %u", prop);

			return FALSE;
		}
		
		if (!(priv->properties[prop].flags & TP_PROPERTY_FLAG_READ))
		{
			g_debug("%s: not allowed to read property %u", G_STRFUNC, prop);

			*error = g_error_new(TELEPATHY_ERRORS, PermissionDenied, "not allowed to read property %u", prop);

			return FALSE;;
		}
	}

	*ret = g_ptr_array_sized_new(properties->len);

	for (i=0; i<properties->len; i++)
	{
		IdleMUCChannelTPProperty prop = g_array_index(properties, guint, i);
		GValue prop_val = {0, };

		g_value_init(&prop_val, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_take_boxed(&prop_val,
				dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_VALUE_STRUCT));

		dbus_g_type_struct_set(&prop_val,
								0, prop,
								1, priv->properties[prop].value,
								G_MAXUINT);

		g_ptr_array_add(*ret, g_value_get_boxed(&prop_val));
	}
	
	return TRUE;
}


/**
 * idle_muc_channel_get_remote_pending_members
 *
 * Implements DBus method GetRemotePendingMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_remote_pending_members (IdleMUCChannel *obj, GArray ** ret, GError **error)
{
	IdleMUCChannelPrivate *priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = idle_handle_set_to_array(priv->remote_pending);
	
	return TRUE;
}


/**
 * idle_muc_channel_get_self_handle
 *
 * Implements DBus method GetSelfHandle
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_get_self_handle (IdleMUCChannel *obj, guint* ret, GError **error)
{
	IdleMUCChannelPrivate *priv;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	g_debug("%s: returning handle %u", G_STRFUNC, priv->own_handle);

	*ret = priv->own_handle;
	
	return TRUE;
}


/**
 * idle_muc_channel_list_pending_messages
 *
 * Implements DBus method ListPendingMessages
 * on interface org.freedesktop.Telepathy.Channel.Type.Text
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_list_pending_messages (IdleMUCChannel *obj,
												 gboolean clear,
												 GPtrArray ** ret,
												 GError **error)
{
	IdleMUCChannelPrivate *priv;
	guint count;
	GPtrArray *messages;
	GList *cur;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	count = g_queue_get_length(priv->pending_messages);

	messages = g_ptr_array_sized_new(count);

	for (cur = g_queue_peek_head_link(priv->pending_messages); cur != NULL; cur = cur->next)
	{
		IdleMUCPendingMessage *msg = (IdleMUCPendingMessage *)(cur->data);
		GValueArray *vals;

		vals = g_value_array_new(6);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 0), G_TYPE_UINT);
		g_value_set_uint(g_value_array_get_nth(vals, 0), msg->id);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 1), G_TYPE_UINT);
		g_value_set_uint(g_value_array_get_nth(vals, 1), msg->timestamp);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 2), G_TYPE_UINT);
		g_value_set_uint(g_value_array_get_nth(vals, 2), msg->sender);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 3), G_TYPE_UINT);
		g_value_set_uint(g_value_array_get_nth(vals, 3), msg->type);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 4), G_TYPE_UINT);
		g_value_set_uint(g_value_array_get_nth(vals, 4), 0);

		g_value_array_append(vals, NULL);
		g_value_init(g_value_array_get_nth(vals, 5), G_TYPE_STRING);
		g_value_set_string(g_value_array_get_nth(vals, 5), msg->text);

		g_ptr_array_add(messages, vals);
	}

	*ret = messages;

	if (clear)
	{
		IdleMUCPendingMessage *msg;

		while ((msg = g_queue_pop_head(priv->pending_messages)) != NULL)
		{
			_idle_muc_pending_free(msg);
		}
	}
	
	return TRUE;
}


/**
 * idle_muc_channel_list_properties
 *
 * Implements DBus method ListProperties
 * on interface org.freedesktop.Telepathy.Properties
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_list_properties (IdleMUCChannel *obj, GPtrArray ** ret, GError **error)
{
	IdleMUCChannelPrivate *priv;
	guint i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	*ret = g_ptr_array_sized_new(LAST_TP_PROPERTY_ENUM);
	
	for (i=0; i<LAST_TP_PROPERTY_ENUM; i++)
	{
		GValue prop = {0, };
		const gchar *dbus_sig;
		const gchar *name;
		guint flags;

		switch (property_signatures[i].type)
		{
			case G_TYPE_BOOLEAN:
			{
				dbus_sig = "b";
			}
			break;
			case G_TYPE_UINT:
			{
				dbus_sig = "u";
			}
			break;
			case G_TYPE_STRING:
			{
				dbus_sig = "s";
			}
			break;
			default:
			{
				g_debug("%s: encountered unknown type %s", G_STRFUNC, g_type_name(property_signatures[i].type));
				*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "internal error in %s", G_STRFUNC);

				return FALSE;
			}
			break;
		}
		
		g_value_init(&prop, TP_TYPE_PROPERTY_INFO_STRUCT);
		g_value_take_boxed(&prop,
				dbus_g_type_specialized_construct(TP_TYPE_PROPERTY_INFO_STRUCT));

		name = property_signatures[i].name;
		flags = priv->properties[i].flags;

		dbus_g_type_struct_set(&prop,
								0, i,
								1, property_signatures[i].name,
								2, dbus_sig,
								3, priv->properties[i].flags,
								G_MAXUINT);

		g_ptr_array_add(*ret, g_value_get_boxed(&prop));
	}
	
	return TRUE;
}


/**
 * idle_muc_channel_provide_password
 *
 * Implements DBus method ProvidePassword
 * on interface org.freedesktop.Telepathy.Channel.Interface.Password
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_provide_password (IdleMUCChannel *obj, const gchar * password, DBusGMethodInvocation *context)
{
	IdleMUCChannelPrivate *priv;
	GError *error;
	
	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	if (!(priv->password_flags & TP_CHANNEL_PASSWORD_FLAG_PROVIDE) || (priv->passwd_ctx != NULL))
	{
		g_debug("%s: don't need a password now or authentication already in process (handle %u)", G_STRFUNC, priv->handle);

		error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "don't need a password now or authentication already in process (handle %u)", priv->handle);

		dbus_g_method_return_error(context, error);
		g_error_free(error);

		return FALSE;
	}
	
	priv->passwd_ctx = context;

	send_join_request(obj, password);
	
	return TRUE;
}


/**
 * idle_muc_channel_remove_members
 *
 * Implements DBus method RemoveMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_remove_members (IdleMUCChannel *obj, const GArray * contacts, const gchar * message, GError **error)
{
	IdleMUCChannelPrivate *priv;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	for (i=0; i<contacts->len; i++)
	{
		GError *kick_error;
		IdleHandle handle = g_array_index(contacts, guint, i);
		
		if (handle == priv->own_handle)
		{
			part_from_channel(obj, message);

			return TRUE;
		}

		if (!idle_handle_set_contains(priv->current_members, handle))
		{
			g_debug("%s: handle %u not a current member!", G_STRFUNC, handle);

			*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "handle %u is not a current member of the channel", handle);

			return FALSE;
		}

		if (!send_kick_request(obj, handle, message, &kick_error))
		{
			g_debug("%s: send_kick_request failed: %s", G_STRFUNC, kick_error->message);

			*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, kick_error->message);

			g_error_free(kick_error);

			return FALSE;
		}
	}
	
	return TRUE;
}


/**
 * idle_muc_channel_send
 *
 * Implements DBus method Send
 * on interface org.freedesktop.Telepathy.Channel.Type.Text
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_send (IdleMUCChannel *obj, guint type, const gchar * text, GError **error)
{
	IdleMUCChannelPrivate *priv;
	gchar msg[IRC_MSG_MAXLEN+2];
	const char *recipient;
	time_t timestamp;
	const gchar *final_text = text;
	gsize len;
	gchar *part;
	gsize headerlen;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	recipient = idle_handle_inspect(_idle_connection_get_handles(priv->connection), TP_HANDLE_TYPE_ROOM, priv->handle);

	if ((recipient == NULL) || (recipient[0] == '\0'))
	{
		g_debug("%s: invalid recipient (handle %u)", G_STRFUNC, priv->handle);

		*error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "invalid recipient");

		return FALSE;
	}

	len = strlen(final_text);
	part = (gchar*)final_text;

	switch (type)
	{
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL:
		{
			g_snprintf(msg, IRC_MSG_MAXLEN, "PRIVMSG %s :", recipient);
		}
		break;
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION:
		{
			g_snprintf(msg, IRC_MSG_MAXLEN, "PRIVMSG %s :\001ACTION ", recipient);
		}
		break;
		case TP_CHANNEL_TEXT_MESSAGE_TYPE_NOTICE:
		{
			g_snprintf(msg, IRC_MSG_MAXLEN, "NOTICE %s :", recipient);
		}
		break;
		default:
		{
			g_debug("%s: invalid message type %u", G_STRFUNC, type);

			*error = g_error_new(TELEPATHY_ERRORS, InvalidArgument, "invalid message type %u", type);

			return FALSE;
		}
		break;
	}

	headerlen = strlen(msg);

	while (part < final_text+len)
	{
		char *br = strchr (part, '\n');
		size_t len = IRC_MSG_MAXLEN-headerlen;
		if (br)
		{
			len = (len < br - part) ? len : br - part;
		}

		if (type == TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION)
		{
			g_snprintf(msg+headerlen, len + 1, "%s\001", part);
			len -= 1;
		}
		else
		{
			g_strlcpy(msg+headerlen, part, len + 1);
		}
		part += len;
		if (br)
		{
			part++;
		}

		_idle_connection_send(priv->connection, msg);
	}

	timestamp = time(NULL);

	if ((priv->mode_state.flags & MODE_FLAG_MODERATED) && !(priv->mode_state.flags & (MODE_FLAG_OPERATOR_PRIVILEGE|MODE_FLAG_HALFOP_PRIVILEGE|MODE_FLAG_VOICE_PRIVILEGE)))
	{
		g_debug("%s: emitting SEND_ERROR with (%u, %llu, %u, %s)", G_STRFUNC, TP_CHANNEL_TEXT_SEND_ERROR_PERMISSION_DENIED, (guint64)(timestamp), type, text);
		g_signal_emit(obj, signals[SEND_ERROR], 0, TP_CHANNEL_TEXT_SEND_ERROR_PERMISSION_DENIED, timestamp, type, text);
	}
	else
	{
		g_debug("%s: emitting SENT with (%llu, %u, %s)", G_STRFUNC, (guint64)(timestamp), type, text);
		g_signal_emit(obj, signals[SENT], 0, timestamp, type, text);
	}
	
	return TRUE;
}

static char to_irc_mode(IdleMUCChannelTPProperty prop_id)
{
	switch (prop_id)
	{
		case TP_PROPERTY_INVITE_ONLY:
			return 'i';
		case TP_PROPERTY_MODERATED:
			return 'm';
		case TP_PROPERTY_PRIVATE:
			return 's';
		default:
			return '\0';
	}
}

static int prop_arr_find(const GPtrArray *props, IdleMUCChannelTPProperty needle)
{
	int i;

	for (i=0; i<props->len; i++)
	{
		GValue prop = {0, };
		guint prop_id;

		g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_set_static_boxed(&prop, g_ptr_array_index(props, i));

		dbus_g_type_struct_get(&prop,
							   0, &prop_id,
							   G_MAXUINT);

		if (prop_id == needle)
		{
			return i;
		}
	}

	return -1;
}

static void send_properties_request(IdleMUCChannel *obj, const GPtrArray *properties)
{
	IdleMUCChannelPrivate *priv;
	int i;
	GPtrArray *waiting;
	gchar cmd[IRC_MSG_MAXLEN+2];
	size_t len;
	gchar *body;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));
	g_assert(properties != NULL);

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	waiting = g_ptr_array_new();

	g_snprintf(cmd, IRC_MSG_MAXLEN+2, "MODE %s ", priv->channel_name);
	len = strlen(cmd);
	body = cmd+len;

	for (i=0; i<properties->len; i++)
	{
		GValue prop = {0, };
		IdleMUCChannelTPProperty prop_id;
		GValue *prop_val;
		char irc_mode;

		g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_set_static_boxed(&prop, g_ptr_array_index(properties, i));

		dbus_g_type_struct_get(&prop,
								0, &prop_id,
								1, &prop_val,
								G_MAXUINT);

		irc_mode = to_irc_mode(prop_id);

		if (irc_mode != '\0')
		{
			g_assert(G_VALUE_TYPE(prop_val) == G_TYPE_BOOLEAN);
			
			gboolean state = g_value_get_boolean(prop_val);
			
			size_t seq = 0;

			if (state)
			{
				body[seq++] = '+';
			}
			else
			{
				body[seq++] = '-';
			}

			body[seq++] = irc_mode;
			body[seq++] = '\0';

			_idle_connection_send(priv->connection, cmd);
		}
		else
		{
			if (prop_id == TP_PROPERTY_SUBJECT)
			{
				const gchar *subject = g_value_get_string(prop_val);
				gchar cmd[IRC_MSG_MAXLEN+2];

				g_snprintf(cmd, IRC_MSG_MAXLEN+2, "TOPIC %s :%s", priv->channel_name, subject);

				_idle_connection_send(priv->connection, cmd);
			}
			else
			{
				g_ptr_array_add(waiting, g_value_get_boxed(&prop));
			}
		}
	}

	if (waiting->len)
	{
		int i, j;
		gpointer tmp;

		i = prop_arr_find(waiting, TP_PROPERTY_LIMITED);
		j = prop_arr_find(waiting, TP_PROPERTY_LIMIT);

		if ((i != -1) && (j != -1) && (i < j))
		{
			g_debug("%s: swapping order of TP_PROPERTY_LIMIT and TP_PROPERTY_LIMITED", G_STRFUNC);

			tmp = g_ptr_array_index(waiting, i);
			g_ptr_array_index(waiting, i) = g_ptr_array_index(waiting, j);
			g_ptr_array_index(waiting, j) = tmp;
		}
		
		i = prop_arr_find(waiting, TP_PROPERTY_PASSWORD_REQUIRED);
		j = prop_arr_find(waiting, TP_PROPERTY_PASSWORD);

		if ((i != -1) && (j != -1) && (i < j))
		{
			g_debug("%s: swapping order of TP_PROPERTY_PASSWORD and TP_PROPERTY_PASSWORD_REQUIRED", G_STRFUNC);

			tmp = g_ptr_array_index(waiting, i);
			g_ptr_array_index(waiting, i) = g_ptr_array_index(waiting, j);
			g_ptr_array_index(waiting, j) = tmp;
		}
	}
	
	/* okay now the data is ALWAYS before the boolean */

	for (i=0; i<waiting->len; i++)
	{
		GValue prop = {0, };
		IdleMUCChannelTPProperty prop_id;
		GValue *prop_val;

		g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_set_static_boxed(&prop, g_ptr_array_index(waiting, i));

		dbus_g_type_struct_get(&prop,
								0, &prop_id,
								1, &prop_val,
								G_MAXUINT);

		g_assert(prop_id < LAST_TP_PROPERTY_ENUM);

		if (prop_id == TP_PROPERTY_LIMIT || prop_id == TP_PROPERTY_PASSWORD)
		{
			int j;

			g_value_copy(prop_val, priv->properties[prop_id].value);
			
			j = prop_arr_find(waiting, prop_id+1);
			
			if (j == -1)
			{
				if (prop_id == TP_PROPERTY_LIMIT && priv->mode_state.flags & MODE_FLAG_USER_LIMIT)
				{
					g_snprintf(body, IRC_MSG_MAXLEN-len, "+l %u", g_value_get_uint(prop_val));
				}
				else if	(prop_id == TP_PROPERTY_PASSWORD && priv->mode_state.flags & MODE_FLAG_KEY)
				{
					g_snprintf(body, IRC_MSG_MAXLEN-len, "+k %s", g_value_get_string(prop_val));
				}
				else
				{
					g_debug("%s: %u", G_STRFUNC, __LINE__);
				}
			}
			else
			{
				g_debug("%s: %u", G_STRFUNC, __LINE__);
			}
		}
		else if (prop_id == TP_PROPERTY_LIMITED)
		{
			guint limit = g_value_get_uint(priv->properties[TP_PROPERTY_LIMIT].value);

			if (g_value_get_boolean(prop_val))
			{
				if (limit != 0)
				{
					g_snprintf(body, IRC_MSG_MAXLEN-len, "+l %u", limit);
				}
				else
				{
					g_debug("%s: %u", G_STRFUNC, __LINE__);
				}
			}
			else
			{
				g_snprintf(body, IRC_MSG_MAXLEN-len, "-l");
			}
		}
		else if (prop_id == TP_PROPERTY_PASSWORD_REQUIRED)
		{
			const gchar *key = g_value_get_string(priv->properties[TP_PROPERTY_PASSWORD].value);

			if (g_value_get_boolean(prop_val))
			{
				if (key != NULL)
				{
					g_snprintf(body, IRC_MSG_MAXLEN-len, "+k %s", key);
				}
				else
				{
					g_debug("%s: %u", G_STRFUNC, __LINE__);
				}
			}
			else
			{
				g_snprintf(body, IRC_MSG_MAXLEN-len, "-k");
			}
		}
		else
		{
			g_debug("%s: %u", G_STRFUNC, __LINE__);
		}

		_idle_connection_send(priv->connection, cmd);

#if 0
		if (prop_id == TP_PROPERTY_LIMIT 
				&& (prop_arr_has_set(waiting, TP_PROPERTY_LIMITED) 
					|| (priv->mode_state.flags & MODE_FLAG_USER_LIMIT)))
		{
			guint limit = g_value_get_uint(prop_val);

			g_snprintf(body, IRC_MSG_MAXLEN-len, "+l %u", limit);
		}
		else if (prop_id == TP_PROPERTY_PASSWORD
				&& (prop_arr_has_set(waiting, TP_PROPERTY_PASSWORD_REQUIRED)
					|| priv->mode_state.flags & MODE_FLAG_KEY))
		{
			const gchar *key = g_value_get_string(prop_val);

			g_snprintf(body, IRC_MSG_MAXLEN-len, "+k %s", key);
		}
		else
		{
			if (prop_id == TP_PROPERTY_LIMITED && !g_value_get_boolean(prop_val))
			{
				g_snprintf(body, IRC_MSG_MAXLEN-len, "-l");
			}
			else if (prop_id == TP_PROPERTY_PASSWORD_REQUIRED && !g_value_get_boolean(prop_val))
			{
				g_snprintf(body, IRC_MSG_MAXLEN-len, "-k");
			}
			else
			{
				g_debug("%s: did not do anything with %u", G_STRFUNC, prop_id);
				continue;
			}
		}
#endif
	}

	g_ptr_array_free(waiting, TRUE);
}

/**
 * idle_muc_channel_set_properties
 *
 * Implements DBus method SetProperties
 * on interface org.freedesktop.Telepathy.Properties
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occured, DBus will throw the error only if this
 *         function returns false.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean idle_muc_channel_set_properties (IdleMUCChannel *obj, const GPtrArray * properties, GError **error)
{
	IdleMUCChannelPrivate *priv;
	GPtrArray *to_change;
	int i;

	g_assert(obj != NULL);
	g_assert(IDLE_IS_MUC_CHANNEL(obj));

	priv = IDLE_MUC_CHANNEL_GET_PRIVATE(obj);

	to_change = g_ptr_array_new();
	
	for (i=0; i<properties->len; i++)
	{
		GValue prop = {0, };
		IdleMUCChannelTPProperty prop_id;
		GValue *prop_val;

		g_value_init(&prop, TP_TYPE_PROPERTY_VALUE_STRUCT);
		g_value_set_static_boxed(&prop, g_ptr_array_index(properties, i));

		dbus_g_type_struct_get(&prop,
								0, &prop_id,
								1, &prop_val,
								G_MAXUINT);

		if (prop_id >= LAST_TP_PROPERTY_ENUM)
		{
			g_debug("%s: invalid property id %u", G_STRFUNC, prop_id);

			*error = g_error_new(TELEPATHY_ERRORS, InvalidArgument, "invalid property id %u", prop_id);

			return FALSE;
		}

		if ((priv->properties[prop_id].flags & TP_PROPERTY_FLAG_WRITE) == 0)
		{
			g_debug("%s: not allowed to set property with id %u", G_STRFUNC, prop_id);

			*error = g_error_new(TELEPATHY_ERRORS, PermissionDenied, "not allowed to set property with id %u", prop_id);

			return FALSE;
		}

		if (!g_value_type_compatible(G_VALUE_TYPE(prop_val), property_signatures[prop_id].type))
		{
			g_debug("%s: incompatible value type %s for prop_id %u", G_STRFUNC, g_type_name(G_VALUE_TYPE(prop_val)), prop_id);

			*error = g_error_new(TELEPATHY_ERRORS, InvalidArgument, "incompatible value type %s for prop_id %u", g_type_name(G_VALUE_TYPE(prop_val)), prop_id);

			return FALSE;
		}

		if (!g_value_compare(prop_val, priv->properties[prop_id].value))
		{
			g_ptr_array_add(to_change, g_value_get_boxed(&prop));
		}
	}

	send_properties_request(obj, to_change);

	g_ptr_array_free(to_change, TRUE);
	
	return TRUE;
}


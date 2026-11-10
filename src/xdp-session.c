/*
 * Copyright © 2017 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include "xdp-session.h"

#include <string.h>

#include "xdp-context.h"
#include "xdp-request.h"

typedef enum
{
  PROP_CONTEXT = 1,
  PROP_SENDER,
  PROP_APP_ID,
  PROP_TOKEN,
  PROP_CONNECTION,
  PROP_IMPL_CONNECTION,
  PROP_IMPL_DBUS_NAME,
} XdpSessionProps;

static GParamSpec *obj_props[PROP_IMPL_DBUS_NAME + 1];

G_LOCK_DEFINE (sessions);
static GHashTable *sessions;

static void g_initable_iface_init (GInitableIface *iface);
static void xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (XdpSession, xdp_session, XDP_DBUS_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_SESSION,
                                                xdp_session_skeleton_iface_init));

#define XDP_SESSION_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS ((o), xdp_session_get_type (), XdpSessionClass))

const char *
lookup_session_token (GVariant *options)
{
  const char *token = NULL;

  g_variant_lookup (options, "session_handle_token", "&s", &token);

  return token;
}

XdpSession *
xdp_session_from_request (const char *session_handle,
                          XdpRequest *request)
{
  g_autoptr(XdpSession) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (!session || g_atomic_int_get (&session->impl_owner_lost))
    return NULL;

  if (g_strcmp0 (session->sender, request->sender) != 0)
    return NULL;

  if (g_strcmp0 (session->app_id, xdp_app_info_get_id (request->app_info)) != 0)
    return NULL;

  return g_steal_pointer (&session);
}

XdpSession *
xdp_session_from_app_info (const char *session_handle,
                           XdpAppInfo *app_info)
{
  g_autoptr(XdpSession) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (!session || g_atomic_int_get (&session->impl_owner_lost))
    return NULL;

  if (g_strcmp0 (session->sender, xdp_app_info_get_sender (app_info)) != 0)
    return NULL;

  if (g_strcmp0 (session->app_id, xdp_app_info_get_id (app_info)) != 0)
    return NULL;

  return g_steal_pointer (&session);
}

XdpSession *
xdp_session_lookup (const char *session_handle)
{
  g_autoptr(XdpSession) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (session && g_atomic_int_get (&session->impl_owner_lost))
    return NULL;

  return g_steal_pointer (&session);
}

gboolean
xdp_session_export (XdpSession  *session,
                    GError     **error)
{
  if (session->closed)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "Session is already closed");
      return FALSE;
    }

  if (session->impl_name_watch)
    {
      g_autofree char *owner =
        g_dbus_proxy_get_name_owner (session->impl_name_watch);

      if (g_atomic_int_get (&session->impl_owner_lost) ||
          g_strcmp0 (owner, session->impl_owner) != 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                               "Session backend owner has changed");
          return FALSE;
        }
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         session->connection,
                                         session->id,
                                         error))
    return FALSE;

  g_object_ref (session);
  session->exported = TRUE;
  g_atomic_int_set (&session->impl_was_exported, TRUE);

  return TRUE;
}

static void
xdp_session_unexport (XdpSession *session)
{
  session->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  g_object_unref (session);
}

void
xdp_session_register (XdpSession *session)
{
  G_LOCK (sessions);
  g_hash_table_insert (sessions, session->id, session);
  G_UNLOCK (sessions);
}

static void
xdp_session_unregister (XdpSession *session)
{
  G_LOCK (sessions);
  g_hash_table_remove (sessions, session->id);
  G_UNLOCK (sessions);
}

void
xdp_session_close (XdpSession *session,
                   gboolean    notify_closed)
{
  if (session->closed)
    return;

  session->closed = TRUE;
  g_atomic_int_set (&session->impl_owner_lost, TRUE);
  g_clear_object (&session->impl_name_watch);

  XDP_SESSION_GET_CLASS (session)->close (session);

  if (notify_closed)
    {
      g_auto(GVariantBuilder) details_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

      g_dbus_connection_emit_signal (session->connection,
                                     session->sender,
                                     session->id,
                                     DESKTOP_DBUS_IFACE ".Session",
                                     "Closed",
                                     g_variant_new ("(@a{sv})", g_variant_builder_end (&details_builder)),
                                     NULL);
    }

  if (session->exported)
    xdp_session_unexport (session);

  xdp_session_unregister (session);
  xdp_context_unclaim_object_path (session->context, session->id);

  if (session->impl_session)
    {
      g_autoptr(GError) error = NULL;

      if (!xdp_dbus_impl_session_call_close_sync (session->impl_session,
                                                  NULL, &error))
        g_warning ("Failed to close session implementation: %s",
                   error->message);

      g_clear_object (&session->impl_session);
    }

  g_object_unref (session);
}

static gboolean
handle_close (XdpDbusSession *object,
              GDBusMethodInvocation *invocation)
{
  XdpSession *session = XDP_SESSION (object);

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  xdp_session_close (session, FALSE);

  xdp_dbus_session_complete_close (object, invocation);

  return TRUE;
}

static void
xdp_session_skeleton_iface_init (XdpDbusSessionIface *iface)
{
  iface->handle_close = handle_close;
}

static void
on_closed (XdpDbusImplSession *object, GObject *data)
{
  XdpSession *session = XDP_SESSION (data);

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  g_clear_object (&session->impl_session);
  xdp_session_close (session, session->exported);
}

static void
close_orphan_done (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object), res, &error);
  if (!reply)
    g_debug ("Orphaned backend session cleanup: %s", error->message);
}

static void
close_orphan (XdpSession *session)
{
  /* A replaced owner may still be alive and controlling input. Retire that
   * unique owner, never the process which now owns the well-known name. Do not
   * hold up public session revocation if the old backend is unresponsive. */
  g_dbus_connection_call (session->impl_connection,
                          session->impl_owner,
                          session->id,
                          "org.freedesktop.impl.portal.Session", "Close",
                          NULL, G_VARIANT_TYPE_UNIT,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          5000, NULL, close_orphan_done, NULL);
}

static void
close_lost_owner_in_thread (GTask        *task,
                            gpointer      source_object,
                            gpointer      task_data,
                            GCancellable *cancellable)
{
  XdpSession *session = XDP_SESSION (source_object);

  {
    SESSION_AUTOLOCK (session);

    /* A creation callback rejects the latched loss before export and retires
     * its session only after the outstanding backend create has finished. */
    if (!session->closed && session->exported)
      {
        if (!GPOINTER_TO_INT (task_data))
          close_orphan (session);
        g_clear_object (&session->impl_session);
        xdp_session_close (session, TRUE);
      }
  }

  g_task_return_boolean (task, TRUE);
}

static void
on_impl_name_owner_changed (GDBusProxy *proxy,
                            GParamSpec *pspec,
                            XdpSession *session)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);
  g_autoptr(GTask) task = NULL;
  gboolean cleanup_sent;

  if (g_strcmp0 (owner, session->impl_owner) == 0)
    return;

  /* Latch once, including a loss followed by reacquisition before create ends.
   * Admission rejects further use immediately. Never wait for a session lock
   * on the frontend loop: a synchronous backend call may still hold it. */
  if (!g_atomic_int_compare_and_exchange (&session->impl_owner_lost, FALSE, TRUE))
    return;

  cleanup_sent = g_atomic_int_get (&session->impl_was_exported);
  if (cleanup_sent)
    close_orphan (session);

  task = g_task_new (session, NULL, NULL, NULL);
  g_task_set_task_data (task, GINT_TO_POINTER (cleanup_sent), NULL);
  g_task_run_in_thread (task, close_lost_owner_in_thread);
}

static gboolean
xdp_session_authorize_callback (GDBusInterfaceSkeleton *interface,
                                GDBusMethodInvocation  *invocation,
                                gpointer                user_data)
{
  const gchar *session_owner = user_data;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_strcmp0 (sender, session_owner) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed, Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

static void
on_peer_disconnect (XdpContext *context,
                    const char *peer,
                    gpointer    user_data)
{
  XdpSession *session = XDP_SESSION (user_data);

  SESSION_AUTOLOCK (session);

  if (g_strcmp0 (session->sender, peer) != 0)
    return;

  xdp_session_close (session, FALSE);
}

static gboolean
xdp_session_initable_init (GInitable     *initable,
                           GCancellable  *cancellable,
                           GError       **error)
{
  XdpSession *session = XDP_SESSION (initable);
  g_autofree char *sender_escaped = NULL;
  g_autofree char *id = NULL;
  g_autofree char *impl_owner = NULL;
  g_autoptr(XdpDbusImplSession) impl_name_watch = NULL;
  g_autoptr(XdpDbusImplSession) impl_session = NULL;
  int i;

  g_assert (session->sender != NULL);
  g_assert (session->token != NULL);
  g_assert (session->context != NULL);

  sender_escaped = g_strdup (session->sender + 1);
  for (i = 0; sender_escaped[i]; i++)
    {
      if (sender_escaped[i] == '.')
        sender_escaped[i] = '_';
    }

  if (!session->token)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Missing token");
      return FALSE;
    }

  if (!xdp_is_valid_token (session->token))
    {
      g_set_error (error,
                   XDG_DESKTOP_PORTAL_ERROR,
                   XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid token '%s'", session->token);
      return FALSE;
    }

  id = g_strdup_printf (DESKTOP_DBUS_PATH "/session/%s/%s",
                        sender_escaped, session->token);

  while (!xdp_context_claim_object_path (session->context, id))
    {
      g_autofree char *t = xdp_generate_token ();
      g_clear_pointer (&id, g_free);
      id = g_strdup_printf (DESKTOP_DBUS_PATH "/session/%s/%s/%s",
                            sender_escaped,
                            session->token,
                            t);
    }

  if (session->impl_dbus_name)
    {
      impl_name_watch =
        xdp_dbus_impl_session_proxy_new_sync (session->impl_connection,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                              G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                              session->impl_dbus_name,
                                              id,
                                              NULL, error);
      if (!impl_name_watch)
        goto fail_claim;

      impl_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (impl_name_watch));
      if (!impl_owner)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                               "Session backend has no owner");
          goto fail_claim;
        }

      impl_session =
        xdp_dbus_impl_session_proxy_new_sync (session->impl_connection,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                              G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                              impl_owner,
                                              id,
                                              NULL, error);
      if (!impl_session)
        goto fail_claim;

      g_signal_connect_object (impl_session, "closed",
                               G_CALLBACK (on_closed),
                               session,
                               G_CONNECT_DEFAULT);

      session->impl_session = g_steal_pointer (&impl_session);
      session->impl_owner = g_steal_pointer (&impl_owner);
      session->impl_name_watch = G_DBUS_PROXY (g_steal_pointer (&impl_name_watch));
      g_signal_connect_object (session->impl_name_watch, "notify::g-name-owner",
                               G_CALLBACK (on_impl_name_owner_changed),
                               session,
                               G_CONNECT_DEFAULT);
    }

  session->id = g_steal_pointer (&id);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (session),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (session, "g-authorize-method",
                    G_CALLBACK (xdp_session_authorize_callback),
                    session->sender);

  g_signal_connect_object (session->context, "peer-disconnect",
                           G_CALLBACK (on_peer_disconnect),
                           session,
                           G_CONNECT_DEFAULT);

  return TRUE;

fail_claim:
  xdp_context_unclaim_object_path (session->context, id);
  return FALSE;
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = xdp_session_initable_init;
}

static void
xdp_session_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  XdpSession *session = XDP_SESSION (object);

  switch ((XdpSessionProps) prop_id)
    {
    case PROP_CONTEXT:
      session->context = g_value_get_object (value);
      break;

    case PROP_SENDER:
      session->sender = g_strdup (g_value_get_string (value));
      break;

    case PROP_APP_ID:
      session->app_id = g_strdup (g_value_get_string (value));
      break;

    case PROP_TOKEN:
      session->token = g_strdup (g_value_get_string (value));
      break;

    case PROP_CONNECTION:
      g_set_object (&session->connection, g_value_get_object (value));
      break;

    case PROP_IMPL_CONNECTION:
      g_set_object (&session->impl_connection, g_value_get_object (value));
      break;

    case PROP_IMPL_DBUS_NAME:
      session->impl_dbus_name = g_strdup (g_value_get_string (value));
      break;
    }
}

static void
xdp_session_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  XdpSession *session = XDP_SESSION (object);

  switch ((XdpSessionProps) prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, session->context);
      break;

    case PROP_SENDER:
      g_value_set_string (value, session->sender);
      break;

    case PROP_APP_ID:
      g_value_set_string (value, session->app_id);
      break;

    case PROP_TOKEN:
      g_value_set_string (value, session->token);
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, session->connection);
      break;

    case PROP_IMPL_CONNECTION:
      g_value_set_object (value, session->impl_connection);
      break;

    case PROP_IMPL_DBUS_NAME:
      g_value_set_string (value, session->impl_dbus_name);
      break;
    }
}

static void
xdp_session_finalize (GObject *object)
{
  XdpSession *session = XDP_SESSION (object);

  g_assert (!session->id || !g_hash_table_lookup (sessions, session->id));

  g_free (session->sender);
  g_clear_object (&session->connection);

  g_clear_object (&session->impl_connection);
  g_free (session->impl_dbus_name);
  g_free (session->impl_owner);
  g_clear_object (&session->impl_name_watch);
  g_clear_object (&session->impl_session);

  g_free (session->app_id);
  g_free (session->id);
  g_free (session->token);

  g_mutex_clear (&session->mutex);

  G_OBJECT_CLASS (xdp_session_parent_class)->finalize (object);
}

static void
xdp_session_init (XdpSession *session)
{
  g_mutex_init (&session->mutex);
}

static void
xdp_session_class_init (XdpSessionClass *klass)
{
  GObjectClass *gobject_class;

  sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xdp_session_finalize;
  gobject_class->set_property = xdp_session_set_property;
  gobject_class->get_property = xdp_session_get_property;

  obj_props[PROP_CONTEXT] =
    g_param_spec_object ("context",
                         NULL, NULL,
                         XDP_TYPE_CONTEXT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_SENDER] =
    g_param_spec_string ("sender",
                         NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_APP_ID] =
    g_param_spec_string ("app-id",
                         NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_TOKEN] =
    g_param_spec_string ("token",
                         NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_CONNECTION] =
    g_param_spec_object ("connection",
                         NULL, NULL,
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_IMPL_CONNECTION] =
    g_param_spec_object ("impl-connection",
                         NULL, NULL,
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_IMPL_DBUS_NAME] =
    g_param_spec_string ("impl-dbus-name",
                         NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (obj_props), obj_props);
}

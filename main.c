#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <errno.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef O_PATH
#define O_PATH 0
#endif

static GDBusConnection *bus;
static GMainLoop *loop;
static gboolean wait;
static char *transfer_key;

static gboolean opt_verbose;
static int opt_n_files;
static int opt_n_calls;
static gboolean opt_writable;
static gboolean opt_noautostop;

static const char contents[] = "Like a trash can fire in a prison cell\\\n"
                               "Like a searchlight in the parking lot of hell";

static const char new_contents[] = "I will walk down to the end with you";

static char *
create_file (int k, int i, GError **error)
{
  g_autofree char *filename = NULL;
  g_autofree char *basename = NULL;

  basename = g_strdup_printf ("text_%d_%d.txt", k, i);
  filename = g_build_filename (g_get_user_data_dir (), basename, NULL);

  if (!g_file_set_contents (filename, contents, -1, error))
    return NULL;

  return g_steal_pointer (&filename);
}

static gboolean
verify_file (const char *path)
{
  g_autofree char *data = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;

  if (!g_file_get_contents (path, &data, &length, &error))
    {
      g_error ("Failed to read %s: %s", path, error->message);
      return FALSE;
    }

  g_assert_cmpstr (data, ==, contents);

  if (opt_writable)
    {
      if (!g_file_set_contents (path, new_contents, -1, &error))
        {
          g_error ("Failed to read %s: %s", path, error->message);
          return FALSE;
        }
    }

  return TRUE;
}

static void
response_received (GDBusConnection *bus,
                   const char *sender_name,
                   const char *object_path,
                   const char *interface_name,
                   const char *signal_name,
                   GVariant *parameters,
                   gpointer data)
{
  const char *key;

  g_debug ("Received TransferClosed");

  g_variant_get (parameters, "(&s)", &key);

  g_assert_cmpstr (key, ==, transfer_key);

  g_main_loop_quit (loop);
}

static char *
start_transfer (GError **error)
{
  GVariantBuilder opt_builder;
  g_autofree char *key = NULL;
  g_autoptr(GVariant) ret = NULL;
  int k, i;

  g_debug ("Connecting to TransferClosed");

  g_dbus_connection_signal_subscribe (bus,
                                      "org.freedesktop.portal.Documents",
                                      "org.freedesktop.portal.FileTransfer",
                                      "TransferClosed",
                                      "/org/freedesktop/portal/documents",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                      response_received,
                                      loop,
                                      NULL);

  g_debug ("Calling StartTransfer");

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "writable", g_variant_new_boolean (opt_writable));
  g_variant_builder_add (&opt_builder, "{sv}", "autostop", g_variant_new_boolean (!opt_noautostop));
  ret = g_dbus_connection_call_sync (bus,
                                     "org.freedesktop.portal.Documents",
                                     "/org/freedesktop/portal/documents",
                                     "org.freedesktop.portal.FileTransfer",
                                     "StartTransfer",
                                     g_variant_new ("(a{sv})", &opt_builder),
                                     G_VARIANT_TYPE ("(s)"),
                                     0,
                                     G_MAXINT,
                                     NULL,
                                     error);

  if (ret == NULL)
    return NULL;

  g_variant_get (ret, "(s)", &key);
  g_clear_pointer (&ret, g_variant_unref);

  g_debug ("Received key '%s'", key);

  transfer_key = g_strdup (key);

  for (k = 0; k < opt_n_calls; k++)
    {
      g_autoptr(GUnixFDList) fd_list = NULL;
      GVariantBuilder fds;

      g_variant_builder_init (&fds, G_VARIANT_TYPE ("ah"));
      fd_list = g_unix_fd_list_new ();
      for (i = 0; i < opt_n_files; i++)
        {
          g_autofree char *path = NULL;
          int fd;
          int fd_in;

          path = create_file (k, i, error);
          if (path == NULL)
            return NULL;

          g_debug ("Add file %s", path);

          fd = open (path, (opt_writable ? O_RDWR : O_RDONLY) | O_PATH | O_CLOEXEC);
          if (fd == -1)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Failed to open %s", path);
              return NULL;
            }

          fd_in = g_unix_fd_list_append (fd_list, fd, error);
          close (fd);
          if (fd_in == -1)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to populate fd list");
              return NULL;
            }
          g_variant_builder_add (&fds, "h", fd_in);
        }

      g_debug ("Calling AddFiles");

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      ret = g_dbus_connection_call_with_unix_fd_list_sync (bus,
                                                           "org.freedesktop.portal.Documents",
                                                           "/org/freedesktop/portal/documents",
                                                           "org.freedesktop.portal.FileTransfer",
                                                           "AddFiles",
                                                           g_variant_new ("(saha{sv})", key, &fds, &opt_builder),
                                                           G_VARIANT_TYPE_UNIT,
                                                           0,
                                                           G_MAXINT,
                                                           fd_list,
                                                           NULL,
                                                           NULL,
                                                           error);
      if (ret == NULL)
        return NULL;
    }
  
  return g_steal_pointer (&key);
}

static void
main_a (int argc, char *argv[])
{
  g_autoptr(GError) error = NULL;
  g_autofree char *key = NULL;

  key = start_transfer (&error);
  if (key == NULL)
    {
      g_error ("Starting transfer failed: %s", error->message);
      exit (1);
    }

  g_print ("%s\n", key);

  wait = TRUE;
}

static char **
retrieve_files (const char *key, GError **error)
{
  GVariantBuilder opt_builder;
  g_autoptr(GVariant) ret = NULL;
  char **files = NULL;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  ret = g_dbus_connection_call_sync (bus,
                                     "org.freedesktop.portal.Documents",
                                     "/org/freedesktop/portal/documents",
                                     "org.freedesktop.portal.FileTransfer",
                                     "RetrieveFiles",
                                     g_variant_new ("(sa{sv})", key, &opt_builder),
                                     G_VARIANT_TYPE ("(as)"),
                                     0,
                                     G_MAXINT,
                                     NULL,
                                     error);
  if (ret == NULL)
    return NULL;

  g_variant_get (ret, "(^as)", &files);

  return files;
}

static void
main_b (int argc, char *argv[])
{
  g_autoptr(GError) error = NULL;
  const char *key;
  g_auto(GStrv) files = NULL;
  int i;

  if (argc < 2)
    {
      g_error ("No transfer key specified.\n");
      exit (1);
    }

  key = argv[1];

  files = retrieve_files (key, &error);
  if (files == NULL)
    {
      g_error ("Retrieving files for key %s failed: %s", key, error->message);
      exit (1);
    }

  for (i = 0; files[i]; i++)
    {
      g_debug ("%s", files[i]);
      if (!verify_file (files[i]))
        {
          g_error ("File %s does not have the expected content", files[i]);
          exit (1);
        }
    }

  wait = FALSE;
}

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  printf ("%s\n", message);
}

int
main (int argc, char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *app_id = NULL;
  g_autoptr(GError) error = NULL;
  GOptionEntry entries[] = {
    { "verbose", 0, 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information", NULL },
    { "n-files", 0, 0, G_OPTION_ARG_INT, &opt_n_files, "Number of files", NULL },
    { "n-calls", 0, 0, G_OPTION_ARG_INT, &opt_n_calls, "Number of Add calls", NULL },
    { "writable", 0, 0, G_OPTION_ARG_NONE, &opt_writable, "Make files writable", NULL },
    { "noautostop", 0, 0, G_OPTION_ARG_NONE, &opt_noautostop, "Allow multiple transfers", NULL },
    { NULL }
  };

  loop = g_main_loop_new (NULL, FALSE);

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_error ("%s", error->message);
      exit (1);
    }

  if (opt_n_files == 0)
    opt_n_files = 1;

  if (opt_n_calls == 0)
    opt_n_calls = 1;

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (bus == NULL)
    {
      g_error ("Failed to get session bus: %s", error->message);
      exit (1);
    }

  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, "/.flatpak-info", 0, NULL);
  app_id = g_key_file_get_string (keyfile, "Application", "name", NULL); 

  g_debug ("I am %s", app_id);

  if (strcmp (app_id, "org.flatpak.PortalTestAppA") == 0)
    main_a (argc, argv);
  else
    main_b (argc, argv);

  if (wait)
    g_main_loop_run (loop);

  return 0;
}

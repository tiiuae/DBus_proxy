/*
 * Enhanced GDBus proxy that forwards messages between multiple D-Bus connections:
 * 1. Connects to multiple buses (system, session, or custom sockets)
 * 2. Fetches introspection data from source service
 * 3. Exposes interfaces on target bus
 * 4. Forwards method calls and signals between buses
 */

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GDBusConnection *source_bus;      // Source D-Bus connection
    GDBusConnection *target_bus;      // Target D-Bus connection
    char *source_bus_name;            // Source service name
    char *source_object_path;         // Source object path
    char *proxy_bus_name;             // Proxy service name
    GDBusNodeInfo *introspection_data;
    guint registration_id;
    GList *signal_subscriptions;      // List of signal subscription IDs
} ProxyContext;

static gboolean verbose = FALSE;
static GList *proxy_contexts = NULL;

// Forward method calls from target bus to source bus
static void handle_method_call(GDBusConnection *conn,
                               const char *sender,
                               const char *object_path,
                               const char *interface_name,
                               const char *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data)
{
    ProxyContext *ctx = (ProxyContext *)user_data;
    
    if (verbose) {
        g_print("Forwarding method call: %s.%s from %s to %s\n",
                interface_name, method_name,
                g_dbus_connection_get_unique_name(ctx->target_bus),
                g_dbus_connection_get_unique_name(ctx->source_bus));
    }

    g_dbus_connection_call(
        ctx->source_bus,               // Forward to source bus
        ctx->source_bus_name,          // Destination service
        ctx->source_object_path,       // Object path
        interface_name,                // Interface
        method_name,                   // Method
        parameters,                    // Parameters
        NULL,                          // Expected reply type
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL,                      // Timeout, cancellable
        (GAsyncReadyCallback)[](GObject *source, GAsyncResult *res, gpointer user_data) {
            GDBusMethodInvocation *inv = (GDBusMethodInvocation *)user_data;
            GError *error = NULL;
            GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
            
            if (result) {
                g_dbus_method_invocation_return_value(inv, result);
            } else {
                g_dbus_method_invocation_return_gerror(inv, error);
                g_error_free(error);
            }
        },
        invocation);
}

// Forward signals from source bus to target bus
static void on_signal_received(GDBusConnection *conn,
                               const char *sender_name,
                               const char *object_path,
                               const char *interface_name,
                               const char *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
    ProxyContext *ctx = (ProxyContext *)user_data;
    GError *error = NULL;
    
    if (verbose) {
        g_print("Forwarding signal: %s.%s from %s to %s\n",
                interface_name, signal_name,
                g_dbus_connection_get_unique_name(ctx->source_bus),
                g_dbus_connection_get_unique_name(ctx->target_bus));
    }

    g_dbus_connection_emit_signal(
        ctx->target_bus,               // Emit on target bus
        NULL,                          // destination (broadcast)
        ctx->source_object_path,       // object path
        interface_name,
        signal_name,
        parameters,
        &error);
        
    if (error) {
        g_printerr("Failed to emit signal: %s\n", error->message);
        g_error_free(error);
    }
}

// Get property forwarding
static GVariant *handle_get_property(GDBusConnection *conn,
                                     const char *sender,
                                     const char *object_path,
                                     const char *interface_name,
                                     const char *property_name,
                                     GError **error,
                                     gpointer user_data)
{
    ProxyContext *ctx = (ProxyContext *)user_data;
    
    if (verbose) {
        g_print("Forwarding property get: %s.%s\n", interface_name, property_name);
    }
    
    GVariant *result = g_dbus_connection_call_sync(
        ctx->source_bus,
        ctx->source_bus_name,
        ctx->source_object_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", interface_name, property_name),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, error);
        
    if (result) {
        GVariant *value;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
        return value;
    }
    
    return NULL;
}

// Set property forwarding
static gboolean handle_set_property(GDBusConnection *conn,
                                    const char *sender,
                                    const char *object_path,
                                    const char *interface_name,
                                    const char *property_name,
                                    GVariant *value,
                                    GError **error,
                                    gpointer user_data)
{
    ProxyContext *ctx = (ProxyContext *)user_data;
    
    if (verbose) {
        g_print("Forwarding property set: %s.%s\n", interface_name, property_name);
    }
    
    GVariant *result = g_dbus_connection_call_sync(
        ctx->source_bus,
        ctx->source_bus_name,
        ctx->source_object_path,
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", interface_name, property_name, value),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, error);
        
    if (result) {
        g_variant_unref(result);
        return TRUE;
    }
    
    return FALSE;
}

// Create a D-Bus connection from address
static GDBusConnection *create_connection(const char *address, GError **error)
{
    if (g_strcmp0(address, "system") == 0) {
        return g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    } else if (g_strcmp0(address, "session") == 0) {
        return g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    } else {
        // Custom address (socket, TCP, etc.)
        return g_dbus_connection_new_for_address_sync(
            address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, error);
    }
}

// Setup proxy context
static ProxyContext *setup_proxy(const char *source_address,
                                 const char *target_address,
                                 const char *source_service,
                                 const char *source_path,
                                 const char *proxy_service)
{
    GError *error = NULL;
    ProxyContext *ctx = g_malloc0(sizeof(ProxyContext));
    
    // Connect to source bus
    ctx->source_bus = create_connection(source_address, &error);
    if (!ctx->source_bus) {
        g_printerr("Failed to connect to source bus (%s): %s\n", 
                   source_address, error->message);
        g_error_free(error);
        g_free(ctx);
        return NULL;
    }
    
    // Connect to target bus
    ctx->target_bus = create_connection(target_address, &error);
    if (!ctx->target_bus) {
        g_printerr("Failed to connect to target bus (%s): %s\n", 
                   target_address, error->message);
        g_error_free(error);
        g_object_unref(ctx->source_bus);
        g_free(ctx);
        return NULL;
    }
    
    ctx->source_bus_name = g_strdup(source_service);
    ctx->source_object_path = g_strdup(source_path);
    ctx->proxy_bus_name = g_strdup(proxy_service);
    
    // Introspect source object
    GVariant *xml = g_dbus_connection_call_sync(
        ctx->source_bus,
        ctx->source_bus_name,
        ctx->source_object_path,
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &error);

    if (!xml) {
        g_printerr("Introspection failed for %s: %s\n", source_service, error->message);
        g_error_free(error);
        g_object_unref(ctx->source_bus);
        g_object_unref(ctx->target_bus);
        g_free(ctx->source_bus_name);
        g_free(ctx->source_object_path);
        g_free(ctx->proxy_bus_name);
        g_free(ctx);
        return NULL;
    }

    const char *xml_data;
    g_variant_get(xml, "(s)", &xml_data);
    ctx->introspection_data = g_dbus_node_info_new_for_xml(xml_data, &error);
    g_variant_unref(xml);
    
    if (!ctx->introspection_data) {
        g_printerr("Failed to parse introspection XML: %s\n", error->message);
        g_error_free(error);
        g_object_unref(ctx->source_bus);
        g_object_unref(ctx->target_bus);
        g_free(ctx->source_bus_name);
        g_free(ctx->source_object_path);
        g_free(ctx->proxy_bus_name);
        g_free(ctx);
        return NULL;
    }

    // Setup method/property forwarding
    GDBusInterfaceVTable vtable = {
        .method_call = handle_method_call,
        .get_property = handle_get_property,
        .set_property = handle_set_property
    };

    // Register interfaces and subscribe to signals
    for (int i = 0; ctx->introspection_data->interfaces[i]; i++) {
        GDBusInterfaceInfo *iface = ctx->introspection_data->interfaces[i];
        
        // Register interface on target bus
        ctx->registration_id = g_dbus_connection_register_object(
            ctx->target_bus,
            ctx->source_object_path,
            iface,
            &vtable,
            ctx, NULL, &error);
            
        if (ctx->registration_id == 0) {
            g_printerr("Failed to register interface %s: %s\n", 
                       iface->name, error->message);
            g_error_free(error);
            continue;
        }

        // Subscribe to signals on source bus
        if (iface->signals) {
            for (int j = 0; iface->signals[j]; j++) {
                guint subscription_id = g_dbus_connection_signal_subscribe(
                    ctx->source_bus,
                    ctx->source_bus_name,
                    iface->name,
                    iface->signals[j]->name,
                    ctx->source_object_path,
                    NULL,
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    on_signal_received,
                    ctx,
                    NULL);
                    
                ctx->signal_subscriptions = g_list_prepend(
                    ctx->signal_subscriptions, 
                    GUINT_TO_POINTER(subscription_id));
            }
        }
    }

    // Acquire name on target bus
    g_bus_own_name_on_connection(ctx->target_bus, ctx->proxy_bus_name,
                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                NULL, NULL, NULL, NULL);

    g_print("Proxy setup: %s (%s) -> %s (%s) as %s\n",
            source_service, source_address,
            g_dbus_connection_get_unique_name(ctx->target_bus), target_address,
            proxy_service);
            
    return ctx;
}

// Cleanup proxy context
static void cleanup_proxy(ProxyContext *ctx)
{
    if (!ctx) return;
    
    // Unsubscribe from signals
    for (GList *l = ctx->signal_subscriptions; l; l = l->next) {
        g_dbus_connection_signal_unsubscribe(ctx->source_bus, 
                                           GPOINTER_TO_UINT(l->data));
    }
    g_list_free(ctx->signal_subscriptions);
    
    // Unregister object
    if (ctx->registration_id > 0) {
        g_dbus_connection_unregister_object(ctx->target_bus, ctx->registration_id);
    }
    
    if (ctx->introspection_data) {
        g_dbus_node_info_unref(ctx->introspection_data);
    }
    
    if (ctx->source_bus) {
        g_object_unref(ctx->source_bus);
    }
    
    if (ctx->target_bus) {
        g_object_unref(ctx->target_bus);
    }
    
    g_free(ctx->source_bus_name);
    g_free(ctx->source_object_path);
    g_free(ctx->proxy_bus_name);
    g_free(ctx);
}

static void print_usage(const char *prog_name)
{
    g_print("Usage: %s [options]\n", prog_name);
    g_print("Options:\n");
    g_print("  -v, --verbose              Enable verbose output\n");
    g_print("  -s, --source ADDRESS       Source bus address (system|session|unix:path=...)\n");
    g_print("  -t, --target ADDRESS       Target bus address (system|session|unix:path=...)\n");
    g_print("  -n, --service-name NAME    Source service name\n");
    g_print("  -p, --object-path PATH     Source object path\n");
    g_print("  -x, --proxy-name NAME      Proxy service name\n");
    g_print("\nExample:\n");
    g_print("  %s -s system -t session -n org.freedesktop.NetworkManager \\\n", prog_name);
    g_print("     -p /org/freedesktop/NetworkManager -x org.example.Proxy\n");
}

int main(int argc, char *argv[])
{
    const char *source_address = "system";
    const char *target_address = "session";
    const char *source_service = "org.freedesktop.NetworkManager";
    const char *source_path = "/org/freedesktop/NetworkManager";
    const char *proxy_service = "org.example.Proxy";
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "-v") == 0 || g_strcmp0(argv[i], "--verbose") == 0) {
            verbose = TRUE;
        } else if (g_strcmp0(argv[i], "-s") == 0 || g_strcmp0(argv[i], "--source") == 0) {
            if (++i < argc) source_address = argv[i];
        } else if (g_strcmp0(argv[i], "-t") == 0 || g_strcmp0(argv[i], "--target") == 0) {
            if (++i < argc) target_address = argv[i];
        } else if (g_strcmp0(argv[i], "-n") == 0 || g_strcmp0(argv[i], "--service-name") == 0) {
            if (++i < argc) source_service = argv[i];
        } else if (g_strcmp0(argv[i], "-p") == 0 || g_strcmp0(argv[i], "--object-path") == 0) {
            if (++i < argc) source_path = argv[i];
        } else if (g_strcmp0(argv[i], "-x") == 0 || g_strcmp0(argv[i], "--proxy-name") == 0) {
            if (++i < argc) proxy_service = argv[i];
        } else if (g_strcmp0(argv[i], "-h") == 0 || g_strcmp0(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Setup proxy
    ProxyContext *ctx = setup_proxy(source_address, target_address,
                                   source_service, source_path, proxy_service);
    if (!ctx) {
        return 1;
    }
    
    proxy_contexts = g_list_prepend(proxy_contexts, ctx);
    
    // Run main loop
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    
    // Setup signal handlers for cleanup
    g_unix_signal_add(SIGINT, (GSourceFunc)[](gpointer user_data) {
        g_main_loop_quit((GMainLoop *)user_data);
        return G_SOURCE_REMOVE;
    }, loop);
    
    g_unix_signal_add(SIGTERM, (GSourceFunc)[](gpointer user_data) {
        g_main_loop_quit((GMainLoop *)user_data);
        return G_SOURCE_REMOVE;
    }, loop);
    
    g_print("Multi-bus proxy running. Press Ctrl+C to stop.\n");
    g_main_loop_run(loop);
    
    // Cleanup
    for (GList *l = proxy_contexts; l; l = l->next) {
        cleanup_proxy((ProxyContext *)l->data);
    }
    g_list_free(proxy_contexts);
    
    g_main_loop_unref(loop);
    return 0;
}

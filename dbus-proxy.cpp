/*
 * Enhanced Cross-Bus GDBus Proxy that:
 * 1. Connects to two different D-Bus buses (source and target).
 * 2. Fetches introspection data from source service on source bus.
 * 3. Exposes that interface on target bus with proxy name.
 * 4. Forwards method calls from target bus to source bus.
 * 5. Forwards signals from source bus to target bus.
 * 6. Handles properties synchronization between buses.
 */

#include <gio/gio.h>
#include <glib/gprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Configuration structure
typedef struct {
    const char *source_bus_name;
    const char *source_object_path;
    const char *proxy_bus_name;
    GBusType source_bus_type;
    GBusType target_bus_type;
    gboolean verbose;
} ProxyConfig;

// Global state
typedef struct {
    GDBusConnection *source_bus;
    GDBusConnection *target_bus;
    GDBusNodeInfo *introspection_data;
    GHashTable *registered_objects;  // Track registered object IDs
    GHashTable *signal_subscriptions; // Track signal subscription IDs
    ProxyConfig config;
} ProxyState;

static ProxyState *proxy_state = NULL;

// Logging functions
static void log_verbose(const char *format, ...)
{
    if (!proxy_state->config.verbose) return;
    
    va_list args;
    va_start(args, format);
    g_print("[VERBOSE] ");
    g_vprintf(format, args);
    g_print("\n");
    va_end(args);
}

static void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_printerr("[ERROR] ");
    g_vfprintf(stderr, format, args);
    g_printerr("\n");
    va_end(args);
}

static void log_info(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_print("[INFO] ");
    g_vprintf(format, args);
    g_print("\n");
    va_end(args);
}

// Forward method calls from target bus to source bus
static void handle_method_call(GDBusConnection *connection G_GNUC_UNUSED,
                               const char *sender,
                               const char *object_path,
                               const char *interface_name,
                               const char *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data G_GNUC_UNUSED)
{
    log_verbose("Method call: %s.%s from %s object_path=%s", interface_name, method_name, sender, object_path);
    
    // Forward the call to the source bus
    g_dbus_connection_call(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        proxy_state->config.source_object_path,
        interface_name,
        method_name,
        parameters,
        NULL, // Expected reply type (auto-detect)
        G_DBUS_CALL_FLAGS_NONE,
        -1, // Default timeout
        NULL, // Cancellable
        (GAsyncReadyCallback)[](GObject *source, GAsyncResult *res, gpointer user_data) {
            GDBusMethodInvocation *inv = (GDBusMethodInvocation *)user_data;
            GError *error = NULL;
            GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
            
            if (result) {
                log_verbose("Method call successful, returning result");
                g_dbus_method_invocation_return_value(inv, result);
            } else {
                log_error("Method call failed: %s", error ? error->message : "Unknown error");
                g_dbus_method_invocation_return_gerror(inv, error);
                if (error) g_error_free(error);
            }
        },
        invocation);
}

// Handle property get requests
static GVariant *handle_get_property(GDBusConnection *connection G_GNUC_UNUSED,
                                     const char *sender,
                                     const char *object_path G_GNUC_UNUSED,
                                     const char *interface_name,
                                     const char *property_name,
                                     GError **error,
                                     gpointer user_data G_GNUC_UNUSED)
{
    log_verbose("Property get: %s.%s from %s", interface_name, property_name, sender);
    
    // Synchronously get property from source bus
    GVariant *result = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        proxy_state->config.source_object_path,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", interface_name, property_name),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        error);
    
    if (result) {
        GVariant *value;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
        log_verbose("Property get successful");
        return value;
    }
    
    log_error("Property get failed: %s", error && *error ? (*error)->message : "Unknown error");
    return NULL;
}

// Handle property set requests
static gboolean handle_set_property(GDBusConnection *connection G_GNUC_UNUSED,
                                    const char *sender,
                                    const char *object_path G_GNUC_UNUSED,
                                    const char *interface_name,
                                    const char *property_name,
                                    GVariant *value,
                                    GError **error,
                                    gpointer user_data G_GNUC_UNUSED)
{
    log_verbose("Property set: %s.%s from %s", interface_name, property_name, sender);
    
    // Forward property set to source bus
    GVariant *result = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        proxy_state->config.source_object_path,
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", interface_name, property_name, value),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        error);
    
    if (result) {
        g_variant_unref(result);
        log_verbose("Property set successful");
        return TRUE;
    }
    
    log_error("Property set failed: %s", error && *error ? (*error)->message : "Unknown error");
    return FALSE;
}

// Forward signals from source bus to target bus
static void on_signal_received(GDBusConnection *connection G_GNUC_UNUSED,
                               const char *sender_name,
                               const char *object_path G_GNUC_UNUSED,
                               const char *interface_name,
                               const char *signal_name,
                               GVariant *parameters,
                               gpointer user_data G_GNUC_UNUSED)
{
    log_verbose("Signal received: %s.%s from %s", interface_name, signal_name, sender_name);
    
    GError *error = NULL;
    gboolean success = g_dbus_connection_emit_signal(
        proxy_state->target_bus,
        NULL, // Broadcast to all subscribers
        proxy_state->config.source_object_path,
        interface_name,
        signal_name,
        parameters,
        &error);
    
    if (success) {
        log_verbose("Signal forwarded successfully");
    } else {
        log_error("Failed to forward signal: %s", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
    }
}

// Handle properties changed signals specially
static void on_properties_changed(GDBusConnection *connection,
                                  const char *sender_name,
                                  const char *object_path,
                                  const char *interface_name,
                                  const char *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data)
{
    const char *changed_interface;
    g_variant_get_child(parameters, 0, "&s", &changed_interface);
    
    log_verbose("Properties changed signal for interface: %s", changed_interface);
    
    // Forward the PropertiesChanged signal
    on_signal_received(connection, sender_name, object_path, interface_name, signal_name, parameters, user_data);
}

// Initialize proxy state
static gboolean init_proxy_state(const ProxyConfig *config)
{
    proxy_state = g_new0(ProxyState, 1);
    proxy_state->config = *config;
    proxy_state->registered_objects = g_hash_table_new(g_direct_hash, g_direct_equal);
    proxy_state->signal_subscriptions = g_hash_table_new(g_direct_hash, g_direct_equal);
    
    return TRUE;
}

// Connect to both buses
static gboolean connect_to_buses()
{
    GError *error = NULL;
    
    // Connect to source bus
    proxy_state->source_bus = g_bus_get_sync(proxy_state->config.source_bus_type, NULL, &error);
    if (!proxy_state->source_bus) {
        log_error("Failed to connect to source bus: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    log_info("Connected to source bus (%s)", 
             proxy_state->config.source_bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
    
    // Connect to target bus
    proxy_state->target_bus = g_bus_get_sync(proxy_state->config.target_bus_type, NULL, &error);
    if (!proxy_state->target_bus) {
        log_error("Failed to connect to target bus: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    log_info("Connected to target bus (%s)", 
             proxy_state->config.target_bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
    
    return TRUE;
}

// Fetch introspection data from source service
static gboolean fetch_introspection_data()
{
    GError *error = NULL;
    
    log_info("Fetching introspection data from %s%s", 
             proxy_state->config.source_bus_name, 
             proxy_state->config.source_object_path);
    
    GVariant *xml_variant = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        proxy_state->config.source_object_path,
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);
    
    if (!xml_variant) {
        log_error("Introspection failed: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    const char *xml_data;
    g_variant_get(xml_variant, "(s)", &xml_data);
    
    log_verbose("Introspection XML received (%zu bytes)", strlen(xml_data));
    
    proxy_state->introspection_data = g_dbus_node_info_new_for_xml(xml_data, &error);
    g_variant_unref(xml_variant);
    
    if (!proxy_state->introspection_data) {
        log_error("Failed to parse introspection XML: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    log_info("Introspection data parsed successfully");
    return TRUE;
}

// Register interfaces and set up signal forwarding
static gboolean setup_proxy_interfaces()
{
    if (!proxy_state->introspection_data->interfaces) {
        log_error("No interfaces found in introspection data");
        return FALSE;
    }
    
    GDBusInterfaceVTable vtable = {
        .method_call = handle_method_call,
        .get_property = handle_get_property,
        .set_property = handle_set_property,
        .padding = {0} // Initialize padding array
    };
    
    // Register each interface on the target bus
    for (int i = 0; proxy_state->introspection_data->interfaces[i]; i++) {
        GDBusInterfaceInfo *iface = proxy_state->introspection_data->interfaces[i];
        GError *error = NULL;
        
        log_info("Registering interface: %s", iface->name);
        
        guint registration_id = g_dbus_connection_register_object(
            proxy_state->target_bus,
            proxy_state->config.source_object_path,
            iface,
            &vtable,
            NULL, // user_data
            NULL, // user_data_free_func
            &error);
        
        if (registration_id == 0) {
            log_error("Failed to register interface %s: %s", iface->name, error->message);
            g_error_free(error);
            return FALSE;
        }
        
        g_hash_table_insert(proxy_state->registered_objects, 
                           GUINT_TO_POINTER(registration_id), 
                           g_strdup(iface->name));
        
        // Subscribe to all signals for this interface
        if (iface->signals) {
            for (int j = 0; iface->signals[j]; j++) {
                log_verbose("Subscribing to signal: %s.%s", iface->name, iface->signals[j]->name);
                
                guint subscription_id = g_dbus_connection_signal_subscribe(
                    proxy_state->source_bus,
                    proxy_state->config.source_bus_name,
                    iface->name,
                    iface->signals[j]->name,
                    proxy_state->config.source_object_path,
                    NULL, // arg0
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    on_signal_received,
                    NULL, // user_data
                    NULL); // user_data_free_func
                
                g_hash_table_insert(proxy_state->signal_subscriptions,
                                   GUINT_TO_POINTER(subscription_id),
                                   g_strdup_printf("%s.%s", iface->name, iface->signals[j]->name));
            }
        }
    }
    
    // Subscribe to PropertiesChanged signals
    guint props_subscription_id = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        proxy_state->config.source_object_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed,
        NULL,
        NULL);
    
    g_hash_table_insert(proxy_state->signal_subscriptions,
                       GUINT_TO_POINTER(props_subscription_id),
                       g_strdup("org.freedesktop.DBus.Properties.PropertiesChanged"));
    
    log_info("All interfaces registered and signal subscriptions set up");
    return TRUE;
}

// ...existing code...
static void on_bus_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    log_info("Name acquired: %s", name);
}

static void on_bus_name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    log_error("Name lost or failed to acquire: %s", name);
}

static gboolean acquire_bus_name()
{
    log_info("Acquiring bus name: %s", proxy_state->config.proxy_bus_name);

    // Use the already-open target connection so the name owner and registered objects share the same connection.
    guint owner_id = g_bus_own_name_on_connection(
        G_DBUS_CONNECTION(proxy_state->target_bus),
        proxy_state->config.proxy_bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_name_acquired,
        NULL, // name_acquired_handler (we use on_bus_name_acquired)
        on_bus_name_lost,
        NULL, // user_data
        NULL  // user_data_free_func
    );

    if (owner_id == 0) {
        log_error("Failed to start owning bus name: %s", proxy_state->config.proxy_bus_name);
        return FALSE;
    }

    log_info("Started owning process for bus name (owner id %u)", owner_id);
    return TRUE;
}

// Cleanup function
static void cleanup_proxy_state()
{
    if (!proxy_state) return;
    
    // Unregister objects
    if (proxy_state->registered_objects) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, proxy_state->registered_objects);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_dbus_connection_unregister_object(proxy_state->target_bus, GPOINTER_TO_UINT(key));
            g_free(value);
        }
        g_hash_table_destroy(proxy_state->registered_objects);
    }
    
    // Unsubscribe from signals
    if (proxy_state->signal_subscriptions) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, proxy_state->signal_subscriptions);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_dbus_connection_signal_unsubscribe(proxy_state->source_bus, GPOINTER_TO_UINT(key));
            g_free(value);
        }
        g_hash_table_destroy(proxy_state->signal_subscriptions);
    }
    
    if (proxy_state->introspection_data) {
        g_dbus_node_info_unref(proxy_state->introspection_data);
    }
    
    if (proxy_state->source_bus) {
        g_object_unref(proxy_state->source_bus);
    }
    
    if (proxy_state->target_bus) {
        g_object_unref(proxy_state->target_bus);
    }
    
    g_free(proxy_state);
    proxy_state = NULL;
}

// Signal handler for graceful shutdown
static void signal_handler(int signum)
{
    log_info("Received signal %d, shutting down...", signum);
    cleanup_proxy_state();
    exit(0);
}

// Parse bus type from string
static GBusType parse_bus_type(const char *bus_str)
{
    if (g_strcmp0(bus_str, "system") == 0) {
        return G_BUS_TYPE_SYSTEM;
    } else if (g_strcmp0(bus_str, "session") == 0) {
        return G_BUS_TYPE_SESSION;
    }
    return G_BUS_TYPE_SYSTEM; // Default
}

// Print usage information
static void print_usage(const char *program_name)
{
    g_print("Usage: %s [OPTIONS]\n", program_name);
    g_print("Cross-bus D-Bus proxy that forwards method calls and signals between buses.\n\n");
    g_print("Options:\n");
    g_print("  --source-bus-name NAME     Source service bus name (example: org.freedesktop.NetworkManager)\n");
    g_print("  --source-object-path PATH  Source object path (example: /org/freedesktop/NetworkManager)\n");
    g_print("  --proxy-bus-name NAME      Proxy bus name (example: org.example.Proxy)\n");
    g_print("  --source-bus-type TYPE     Source bus type: system|session (default: system)\n");
    g_print("  --target-bus-type TYPE     Target bus type: system|session (default: session)\n");
    g_print("  --verbose                  Enable verbose logging\n");
    g_print("  --help                     Show this help message\n");
}

// Validate required proxy configuration parameters
void validateProxyConfigOrExit(const ProxyConfig& config) 
{
    if (!config.source_bus_name || !strlen(config.source_bus_name)) {
        log_error("Error: source_bus_name is required!\n");
        exit(EXIT_FAILURE);
    }    
    if (!config.source_object_path || !strlen(config.source_object_path)) {
        log_error("Error: source_object_path is required!\n");
        exit(EXIT_FAILURE);
    }
    if (!config.proxy_bus_name || !strlen(config.proxy_bus_name)) {
        log_error("Error: proxy_bus_name is required!\n");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    // Default configuration
    ProxyConfig config = {
        .source_bus_name = "",
        .source_object_path = "", 
        .proxy_bus_name = "",
        .source_bus_type = G_BUS_TYPE_SYSTEM,
        .target_bus_type = G_BUS_TYPE_SESSION,
        .verbose = FALSE
    };
    
    // Parse command line arguments
    for (int i = 0; i < argc; i++) {
        if (g_strcmp0(argv[i], "--source-bus-name") == 0 && i + 1 < argc) {
            config.source_bus_name = argv[++i];
        } else if (g_strcmp0(argv[i], "--source-object-path") == 0 && i + 1 < argc) {
            config.source_object_path = argv[++i];
        } else if (g_strcmp0(argv[i], "--proxy-bus-name") == 0 && i + 1 < argc) {
            config.proxy_bus_name = argv[++i];
        } else if (g_strcmp0(argv[i], "--source-bus-type") == 0 && i + 1 < argc) {
            config.source_bus_type = parse_bus_type(argv[++i]);
        } else if (g_strcmp0(argv[i], "--target-bus-type") == 0 && i + 1 < argc) {
            config.target_bus_type = parse_bus_type(argv[++i]);
        } else if (g_strcmp0(argv[i], "--verbose") == 0) {
            config.verbose = TRUE;
        } else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0 || argc == 1) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Validate configuration
    validateProxyConfigOrExit(config);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    log_info("Starting cross-bus D-Bus proxy");
    log_info("Source: %s%s on %s bus", 
             config.source_bus_name, 
             config.source_object_path,
             config.source_bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
    log_info("Target: %s on %s bus", 
             config.proxy_bus_name,
             config.target_bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
    
    // Initialize proxy state
    if (!init_proxy_state(&config)) {
        log_error("Failed to initialize proxy state");
        return 1;
    }
    
    // Connect to both buses
    if (!connect_to_buses()) {
        cleanup_proxy_state();
        return 1;
    }
    
    // Fetch introspection data from source
    if (!fetch_introspection_data()) {
        cleanup_proxy_state();
        return 1;
    }
    
    // Set up proxy interfaces and signal forwarding
    if (!setup_proxy_interfaces()) {
        cleanup_proxy_state();
        return 1;
    }
    
    // Acquire bus name on target bus
    if (!acquire_bus_name()) {
        cleanup_proxy_state();
        return 1;
    }
    
    log_info("Cross-bus proxy is running and ready to forward calls");
    log_info("Press Ctrl+C to stop");
    
    // Run main loop
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    
    // Cleanup
    g_main_loop_unref(loop);
    cleanup_proxy_state();
    
    return 0;
}
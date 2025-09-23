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
    guint name_owner_watch_id;
    guint catch_all_subscription_id; // For catching all signals
    GHashTable *proxied_objects; // object_path -> ProxiedObject*
} ProxyState;

// Structure to track proxied objects
typedef struct {
    char *object_path;
    GDBusNodeInfo *node_info;
    GHashTable *registration_ids; // interface_name -> registration_id
} ProxiedObject;
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

static gboolean proxy_single_object(const char *object_path, GDBusNodeInfo *node_info);

// Free function for ProxiedObject
static void free_proxied_object(gpointer data)
{
    ProxiedObject *obj = (ProxiedObject*)data;
    if (!obj) return;
    
    g_free(obj->object_path);
    if (obj->node_info) {
        g_dbus_node_info_unref(obj->node_info);
    }
    if (obj->registration_ids) {
        g_hash_table_destroy(obj->registration_ids);
    }
    g_free(obj);
}

// Recursively discover and proxy all objects starting from a base path
static gboolean discover_and_proxy_object_tree(const char *base_path)
{
    GError *error = NULL;
    
    log_info("Discovering object tree starting from: %s", base_path);
    
    // Get introspection data for this path
    GVariant *xml_variant = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        base_path,
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        10000, // 10 second timeout - increased for slow systems
        NULL,
        &error);
    
    if (!xml_variant) {
        // Some objects might not be introspectable, that's ok
        log_verbose("Could not introspect %s: %s", base_path, error ? error->message : "Unknown error");
        if (error) {
            // Only log as error if it's not a simple "no such object" error
            if (error->domain == G_DBUS_ERROR && error->code == G_DBUS_ERROR_UNKNOWN_OBJECT) {
                log_verbose("Object %s does not exist, skipping", base_path);
            } else {
                log_error("Introspection error for %s: %s", base_path, error->message);
            }
            g_error_free(error);
        }
        return TRUE; // Continue with other objects
    }
    
    const char *xml_data;
    g_variant_get(xml_variant, "(s)", &xml_data);
    
    log_verbose("Introspection XML for %s (%zu bytes):\n%s", base_path, strlen(xml_data), xml_data);
    
    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(xml_data, &error);
    g_variant_unref(xml_variant);
    
    if (!node_info) {
        log_error("Failed to parse introspection XML for %s: %s", base_path, error ? error->message : "Unknown");
        if (error) g_error_free(error);
        return FALSE;
    }
    
    // Show what interfaces we found
    if (node_info->interfaces) {
        for (int i = 0; node_info->interfaces[i]; i++) {
            log_verbose("Found interface: %s", node_info->interfaces[i]->name);
        }
    }
    
    // Show what child nodes we found
    if (node_info->nodes) {
        for (int i = 0; node_info->nodes[i]; i++) {
            const char *child_name = node_info->nodes[i]->path;
            log_verbose("Found child node: %s", child_name ? child_name : "(unnamed)");
        }
    } else {
        log_verbose("No child nodes found for %s", base_path);
    }
    
    // Proxy this object if it has interfaces
    if (!proxy_single_object(base_path, node_info)) {
        g_dbus_node_info_unref(node_info);
        return FALSE;
    }
    
    // Recursively handle child nodes
    if (node_info->nodes) {
        for (int i = 0; node_info->nodes[i]; i++) {
            const char *child_name = node_info->nodes[i]->path;
            if (!child_name || strlen(child_name) == 0) {
                log_verbose("Skipping unnamed child node");
                continue;
            }
            
            // Build full child path
            char *child_path;
            if (g_str_has_suffix(base_path, "/")) {
                child_path = g_strdup_printf("%s%s", base_path, child_name);
            } else {
                child_path = g_strdup_printf("%s/%s", base_path, child_name);
            }
            
            log_verbose("Recursively processing child: %s", child_path);
            
            // Recurse into child (don't fail if child fails)
            discover_and_proxy_object_tree(child_path);
            
            g_free(child_path);
        }
    }
    
    g_dbus_node_info_unref(node_info);
    return TRUE;
}

// Generic method call handler that works for any object path
static void handle_method_call_generic(G_GNUC_UNUSED GDBusConnection *connection,
                                      const char *sender,
                                      const char *object_path,
                                      const char *interface_name,
                                      const char *method_name,
                                      GVariant *parameters,
                                      GDBusMethodInvocation *invocation,
                                      gpointer user_data)
{
    const char *target_object_path = (const char*)user_data;
    
    log_verbose("Method call: %s.%s on %s from %s (forwarding to %s)", 
                interface_name, method_name, object_path, sender, target_object_path);

    // Take a reference to ensure invocation stays alive
    g_object_ref(invocation);

    // Forward the call to the source bus using the original object path
    g_dbus_connection_call(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        target_object_path, // Use the original object path from source bus
        interface_name,
        method_name,
        parameters,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
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
            // Release our reference
            g_object_unref(inv);
        },
        invocation);
}

// Generic property handlers that work for any object path  
static GVariant *handle_get_property_generic(G_GNUC_UNUSED GDBusConnection *connection,
                                            const char *sender,
                                            const char *object_path,
                                            const char *interface_name,
                                            const char *property_name,
                                            GError **error,
                                            gpointer user_data)
{
    const char *target_object_path = (const char*)user_data;
    
    log_verbose("Property get: %s.%s on %s from %s (forwarding to %s)", 
                interface_name, property_name, object_path, sender, target_object_path);
    
    GVariant *result = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        target_object_path,
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

static gboolean handle_set_property_generic(G_GNUC_UNUSED GDBusConnection *connection,
                                           const char *sender,
                                           const char *object_path,
                                           const char *interface_name,
                                           const char *property_name,
                                           GVariant *value,
                                           GError **error,
                                           gpointer user_data)
{
    const char *target_object_path = (const char*)user_data;
    
    log_verbose("Property set: %s.%s on %s from %s (forwarding to %s)", 
                interface_name, property_name, object_path, sender, target_object_path);
    
    GVariant *result = g_dbus_connection_call_sync(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        target_object_path,
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
        return TRUE;
    }

    log_error("Property set failed: %s", error && *error ? (*error)->message : "Unknown error");
    return FALSE;
}

// Proxy a single object with all its interfaces
static gboolean proxy_single_object(const char *object_path, GDBusNodeInfo *node_info)
{
    // Skip if no interfaces to proxy
    if (!node_info->interfaces || !node_info->interfaces[0]) {
        log_verbose("Object %s has no interfaces, skipping", object_path);
        return TRUE;
    }
    
    log_info("Proxying object: %s", object_path);
    
    // Create proxied object structure
    ProxiedObject *proxied_obj = g_new0(ProxiedObject, 1);
    proxied_obj->object_path = g_strdup(object_path);
    proxied_obj->node_info = g_dbus_node_info_ref(node_info);
    proxied_obj->registration_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    
    GDBusInterfaceVTable vtable = {
        .method_call = handle_method_call_generic,
        .get_property = handle_get_property_generic,
        .set_property = handle_set_property_generic,
        .padding = {0}
    };
    
    // List of standard D-Bus interfaces that GDBus provides automatically
    const char *standard_interfaces[] = {
        "org.freedesktop.DBus.Introspectable",
        "org.freedesktop.DBus.Peer", 
        "org.freedesktop.DBus.Properties",
        "org.freedesktop.DBus.ObjectManager",
        NULL
    };
    
    // Function to check if interface is standard
    auto is_standard_interface = [](const char *interface_name, const char **standard_list) -> gboolean {
        for (int i = 0; standard_list[i]; i++) {
            if (g_strcmp0(interface_name, standard_list[i]) == 0) {
                return TRUE;
            }
        }
        return FALSE;
    };
    
    int registered_count = 0;
    
    // Register each interface (except standard ones)
    for (int i = 0; node_info->interfaces[i]; i++) {
        GDBusInterfaceInfo *iface = node_info->interfaces[i];
        GError *error = NULL;
        
        // Skip standard D-Bus interfaces - GDBus provides these automatically
        if (is_standard_interface(iface->name, standard_interfaces)) {
            log_verbose("Skipping standard interface: %s", iface->name);
            continue;
        }
        
        log_verbose("Registering interface %s on object %s", iface->name, object_path);
        
        guint registration_id = g_dbus_connection_register_object(
            proxy_state->target_bus,
            object_path,
            iface,
            &vtable,
            g_strdup(object_path), // Pass object path as user_data for forwarding
            g_free,
            &error);
        
        if (registration_id == 0) {
            log_error("Failed to register interface %s on %s: %s", 
                     iface->name, object_path, error ? error->message : "Unknown error");
            if (error) g_error_free(error);
            continue; // Try other interfaces
        }
        
        registered_count++;
        
        // Store registration ID
        g_hash_table_insert(proxied_obj->registration_ids, 
                           g_strdup(iface->name), 
                           GUINT_TO_POINTER(registration_id));
        
        // Also add to global registry for cleanup
        g_hash_table_insert(proxy_state->registered_objects,
                           GUINT_TO_POINTER(registration_id),
                           g_strdup_printf("%s:%s", object_path, iface->name));
        
        log_verbose("Interface %s registered on %s with ID %u", iface->name, object_path, registration_id);
    }
    
    if (registered_count > 0) {
        // Store the proxied object only if we registered something
        g_hash_table_insert(proxy_state->proxied_objects, g_strdup(object_path), proxied_obj);
        log_info("Successfully proxied object %s with %d interfaces", object_path, registered_count);
    } else {
        // No interfaces registered, clean up
        log_verbose("No custom interfaces registered for %s", object_path);
        free_proxied_object(proxied_obj);
    }
    
    return TRUE;
}

// Forward signals from source bus to target bus - catch-all version
static void on_signal_received_catchall(GDBusConnection *connection G_GNUC_UNUSED,
                                        const char *sender_name,
                                        const char *object_path,
                                        const char *interface_name,
                                        const char *signal_name,
                                        GVariant *parameters,
                                        gpointer user_data G_GNUC_UNUSED)
{
    // Check if this is a path we're proxying
    if (g_hash_table_contains(proxy_state->proxied_objects, object_path) ||
        g_str_has_prefix(object_path, proxy_state->config.source_object_path) ||
        g_strcmp0(object_path, "/org/freedesktop/DBus") == 0) {
        
        log_verbose("Signal received: %s.%s from %s at %s", 
                    interface_name, signal_name, sender_name, object_path);
        
        GError *error = NULL;
        gboolean success = g_dbus_connection_emit_signal(
            proxy_state->target_bus,
            NULL,
            object_path,
            interface_name,
            signal_name,
            parameters,
            &error);
        
        if (!success) {
            log_error("Failed to forward signal: %s", error ? error->message : "Unknown error");
            if (error) g_error_free(error);
        }
    } else {
        log_error("Signal %s.%s from %s at %s ignored (not proxied)", interface_name, signal_name, sender_name, object_path);
    }
}

// Initialize proxy state
static gboolean init_proxy_state(const ProxyConfig *config)
{
    proxy_state = g_new0(ProxyState, 1);
    proxy_state->config = *config;
    proxy_state->registered_objects = g_hash_table_new(g_direct_hash, g_direct_equal);
    proxy_state->signal_subscriptions = g_hash_table_new(g_direct_hash, g_direct_equal);
    proxy_state->catch_all_subscription_id = 0;
    proxy_state->proxied_objects = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_proxied_object);

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

// Setup signal forwarding with both catch-all and specific PropertiesChanged handling
static gboolean setup_signal_forwarding()
{
    log_info("Setting up signal forwarding");
    
    // Subscribe to ALL signals from the source bus name
    proxy_state->catch_all_subscription_id = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name, // sender (our source service)
        NULL,                                // interface_name (all interfaces)
        NULL,                                // member (all signals)
        NULL,                                // object_path (all paths - we filter in callback)
        NULL,                                // arg0 (no filtering)
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_signal_received_catchall,
        NULL,
        NULL);
    
    if (proxy_state->catch_all_subscription_id == 0) {
        log_error("Failed to set up catch-all signal subscription");
        return FALSE;
    }
    
    log_info("Catch-all signal subscription established (ID: %u)", proxy_state->catch_all_subscription_id);    

    return TRUE;
}

// Register interfaces
static gboolean setup_proxy_interfaces()
{
    log_info("Setting up proxy interfaces - discovering full object tree");

    // First, proxy the D-Bus daemon interface that clients use for service discovery
    if (!discover_and_proxy_object_tree("/org/freedesktop")) {
        log_error("Failed to discover and proxy D-Bus daemon interface");
        return FALSE;
    }

    // Start recursive discovery from the root object
    if (!discover_and_proxy_object_tree(proxy_state->config.source_object_path)) {
        log_error("Failed to discover and proxy object tree");
        return FALSE;
    }
    
    // Set up signal forwarding
    if (!setup_signal_forwarding()) {
        return FALSE;
    }
    
    log_info("Object tree proxying complete - %u objects proxied", 
             g_hash_table_size(proxy_state->proxied_objects));
    
    return TRUE;
}

static void on_bus_acquired_for_owner(GDBusConnection *connection,
                                      const gchar *name,
                                      gpointer user_data G_GNUC_UNUSED)
{
    log_info("Bus acquired for name: %s", name ? name : "(none)");
    if (!proxy_state) return;

    // Keep a reference to the connection
    if (proxy_state->target_bus) {
        g_object_unref(proxy_state->target_bus);
        proxy_state->target_bus = NULL;
    }
    proxy_state->target_bus = g_object_ref(connection);

    // Register interfaces & set up signal forwarding
    if (!setup_proxy_interfaces()) {
        log_error("Failed to set up interfaces on target bus");
        if (proxy_state->name_owner_watch_id) {
            g_bus_unown_name(proxy_state->name_owner_watch_id);
            proxy_state->name_owner_watch_id = 0;
        }
    }
}

static void on_name_acquired_log(G_GNUC_UNUSED GDBusConnection *conn,
                                 const gchar *name,
                                 gpointer user_data G_GNUC_UNUSED)
{
    log_info("Name successfully acquired: %s", name);
}

static void on_name_lost_log(G_GNUC_UNUSED GDBusConnection *conn,
                             const gchar *name,
                             gpointer user_data G_GNUC_UNUSED)
{
    log_error("Name lost or failed to acquire: %s", name);
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
    
    // Unsubscribe from catch-all signal
    if (proxy_state->catch_all_subscription_id && proxy_state->source_bus) {
        g_dbus_connection_signal_unsubscribe(proxy_state->source_bus, proxy_state->catch_all_subscription_id);
        proxy_state->catch_all_subscription_id = 0;
    }
    
    // Clean up individual signal subscriptions (like PropertiesChanged)
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

    if (proxy_state->proxied_objects) {
        g_hash_table_destroy(proxy_state->proxied_objects);
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
static void validateProxyConfigOrExit(const ProxyConfig *config) 
{
    if (!config->source_bus_name || !strlen(config->source_bus_name)) {
        log_error("Error: source_bus_name is required!");
        exit(EXIT_FAILURE);
    }    
    if (!config->source_object_path || !strlen(config->source_object_path)) {
        log_error("Error: source_object_path is required!");
        exit(EXIT_FAILURE);
    }
    if (!config->proxy_bus_name || !strlen(config->proxy_bus_name)) {
        log_error("Error: proxy_bus_name is required!");
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
    validateProxyConfigOrExit(&config);
    
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

    // Start owning the proxy name on the target bus
    proxy_state->name_owner_watch_id = g_bus_own_name(
        proxy_state->config.target_bus_type,
        proxy_state->config.proxy_bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired_for_owner,
        on_name_acquired_log,
        on_name_lost_log,
        NULL, NULL);

    // Run main loop
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    // Cleanup
    if (proxy_state && proxy_state->name_owner_watch_id) {
        g_bus_unown_name(proxy_state->name_owner_watch_id);
        proxy_state->name_owner_watch_id = 0;
    }

    g_main_loop_unref(loop);
    cleanup_proxy_state();
    
    return 0;
}
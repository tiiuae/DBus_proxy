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
    #if 0
    // jarekk: Handle D-Bus daemon method calls.
    // Maybe it's better to handle all requests to /org/freedesktop/DBus in a normal way?
    // The code below is redundant with setup_proxy_interfaces()...

    // Special case: Route D-Bus daemon calls to the D-Bus daemon on source bus
    if (g_strcmp0(object_path, "/org/freedesktop/DBus") == 0) {
        log_verbose(">>>>>> D-Bus daemon method call: %s.%s from %s (routing to source bus D-Bus daemon)", 
                    interface_name, method_name, sender);
        
        // jarekk
        g_print(">>>> Routing D-Bus daemon call %s.%s to source bus\n", interface_name, method_name);
        g_dbus_connection_call(
            proxy_state->source_bus,
            "org.freedesktop.DBus",          // D-Bus daemon service name
            "/org/freedesktop/DBus",         // D-Bus daemon object path  
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
                    log_verbose("D-Bus daemon method call successful");
                    g_dbus_method_invocation_return_value(inv, result);
                } else {
                    log_error("D-Bus daemon method call failed: %s", error ? error->message : "Unknown error");
                    g_dbus_method_invocation_return_gerror(inv, error);
                    if (error) g_error_free(error);
                }
            },
            invocation);
        return;
    }
    #endif

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
        },
        invocation);
}
#if 1
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
        return value;
    }
    
    return NULL;
}
#else

// Generic callback for forwarding replies back to the target bus
static void
forward_call_cb(GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
    GDBusMethodInvocation *invocation = (GDBusMethodInvocation *) user_data;
    GVariant *result = NULL;
    GError *error = NULL;

    result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object),
                                           res,
                                           &error);

    if (error) {
        g_dbus_method_invocation_return_gerror(invocation, error);
        g_error_free(error);
        return;
    }

    if (result) {
        // If the remote method returned a tuple, unpack it automatically
        if (g_variant_is_of_type(result, G_VARIANT_TYPE_TUPLE)) {
            g_dbus_method_invocation_return_value(invocation, result);
        } else {
            // Wrap non-tuple in a tuple
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new_tuple(&result, 1));
            g_variant_unref(result);
        }
    } else {
        // No return value (e.g. Set)
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

#if 0
static GVariant *handle_get_property_generic(G_GNUC_UNUSED GDBusConnection *connection,
                                            const char *sender,
                                            const char *object_path,
                                            const char *interface_name,
                                            const char *property_name,
                                            GError **error,
                                            gpointer user_data)
#endif

static GVariant *
handle_get_property_generic(GDBusConnection *connection,
                            const gchar *sender,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *method_name,
                            GVariant *parameters,
                            GDBusMethodInvocation *invocation,
                            gpointer user_data)
{
    ProxyState *proxy_state = (ProxyState *)user_data;
    const gchar *target_object_path = object_path;

    if (g_strcmp0(method_name, "Get") == 0) {
        const gchar *iface, *prop;
        g_variant_get(parameters, "(&s&s)", &iface, &prop);

        g_dbus_connection_call(
            proxy_state->source_bus,
            proxy_state->config.source_bus_name,
            target_object_path,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", iface, prop),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            (GAsyncReadyCallback)forward_call_cb,
            invocation);
    }
    else if (g_strcmp0(method_name, "Set") == 0) {
        const gchar *iface, *prop;
        GVariant *value;
        g_variant_get(parameters, "(&s&s@v)", &iface, &prop, &value);

        g_dbus_connection_call(
            proxy_state->source_bus,
            proxy_state->config.source_bus_name,
            target_object_path,
            "org.freedesktop.DBus.Properties",
            "Set",
            g_variant_new("(ssv)", iface, prop, value),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            (GAsyncReadyCallback)forward_call_cb,
            invocation);
    }
    else if (g_strcmp0(method_name, "GetAll") == 0) {
        const gchar *iface;
        g_variant_get(parameters, "(&s)", &iface);

        g_dbus_connection_call(
            proxy_state->source_bus,
            proxy_state->config.source_bus_name,
            target_object_path,
            "org.freedesktop.DBus.Properties",
            "GetAll",
            g_variant_new("(s)", iface),
            G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            (GAsyncReadyCallback)forward_call_cb,
            invocation);
    }
    else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unhandled method: %s",
                                              method_name);
    }
}
#endif

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

#if 0 // jarekk
// Update your signal forwarding to be less restrictive
static void on_signal_received_catchall_updated(GDBusConnection *connection,
                                               const char *sender_name,
                                               const char *object_path,
                                               const char *interface_name,
                                               const char *signal_name,
                                               GVariant *parameters,
                                               gpointer user_data)
{
    // Only forward signals from our specific source
    if (g_strcmp0(sender_name, proxy_state->config.source_bus_name) != 0) {
        return;
    }
    
    // Check if this object path is one we're proxying
    if (!g_hash_table_contains(proxy_state->proxied_objects, object_path)) {
        // Also check if it's a child of our root path (for dynamic objects)
        if (!g_str_has_prefix(object_path, proxy_state->config.source_object_path)) {
            return;
        }
    }
    
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
    } else {
        log_verbose("Signal forwarded successfully");
    }
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
#endif

// Add this function to debug what signals nm-applet is expecting
static void setup_signal_debugging()
{
    // Debug: Log ALL signals on the source bus for a few minutes
    guint debug_subscription = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        NULL,  // Any sender
        NULL,  // Any interface  
        NULL,  // Any signal
        NULL,  // Any path
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *connection G_GNUC_UNUSED,
           const char *sender_name,
           const char *object_path,
           const char *interface_name, 
           const char *signal_name,
           GVariant *parameters,
           gpointer user_data G_GNUC_UNUSED) {
            
            // Only log NetworkManager-related signals to avoid spam
            if ((sender_name && g_str_has_prefix(sender_name, "org.freedesktop.NetworkManager")) ||
                (object_path && g_str_has_prefix(object_path, "/org/freedesktop/NetworkManager")) ||
                (interface_name && g_str_has_prefix(interface_name, "org.freedesktop.NetworkManager"))) {
                
                log_info("DEBUG - Signal: %s.%s from %s at %s", 
                         interface_name ?: "null",
                         signal_name ?: "null", 
                         sender_name ?: "null",
                         object_path ?: "null");
                
                // Log parameter types too
                if (parameters) {
                    char *params_str = g_variant_print(parameters, TRUE);
                    log_verbose("  Parameters: %s", params_str);
                    g_free(params_str);
                }
            }
        },
        NULL, NULL);
        
    if (debug_subscription) {
        log_info("Signal debugging enabled for NetworkManager signals");
        
        // Auto-disable debug logging after 2 minutes to avoid spam
        g_timeout_add(120000, [](gpointer data) -> gboolean {
            guint sub_id = GPOINTER_TO_UINT(data);
            if (proxy_state && proxy_state->source_bus) {
                g_dbus_connection_signal_unsubscribe(proxy_state->source_bus, sub_id);
                log_info("Signal debugging disabled");
            }
            return FALSE; // one-shot
        }, GUINT_TO_POINTER(debug_subscription));
    }
}

// Call this in setup_signal_forwarding() if you want to debug:
// setup_signal_debugging();  // Add this line at the end of setup_signal_forwarding()

static void on_signal_received_catchall_fixed(GDBusConnection *connection G_GNUC_UNUSED,
                                              const char *sender_name,
                                              const char *object_path,
                                              const char *interface_name,
                                              const char *signal_name,
                                              GVariant *parameters,
                                              gpointer user_data G_GNUC_UNUSED)
{
    // Critical signals that nm-applet needs - be more permissive
    gboolean should_forward = FALSE;
    
    // 1. Signals from our main NetworkManager service
    if (g_strcmp0(sender_name, proxy_state->config.source_bus_name) == 0) {
        should_forward = TRUE;
    }
    
    // 2. D-Bus daemon signals (service appearing/disappearing)
    else if (g_strcmp0(sender_name, "org.freedesktop.DBus") == 0) {
        should_forward = TRUE;
    }
    
    // jarekk fix hardcoded NetworkManager
    // 3. NetworkManager-related signals from ANY sender (important!)
    else if (g_str_has_prefix(object_path ?: "", "/org/freedesktop/NetworkManager")) {
        should_forward = TRUE;
        log_verbose("Forwarding NM object signal from %s", sender_name ?: "unknown");
    }
    
    // 4. NetworkManager interface signals regardless of sender
    else if (g_str_has_prefix(interface_name ?: "", "org.freedesktop.NetworkManager")) {
        should_forward = TRUE;
        log_verbose("Forwarding NM interface signal from %s", sender_name ?: "unknown");
    }
    // 5. D-Bus daemon interface signals regardless of sender (e.g. NameOwnerChanged)
    // jarekk doesn't happen. What for?
    else if (g_str_has_prefix(interface_name ?: "", "org.freedesktop.DBus")) {
        should_forward = TRUE;
        log_verbose("Forwarding D-Bus interface signal from %s!!!", sender_name ?: "unknown");
    }
    
    if (!should_forward) {
        return;
    }
    
    log_verbose("Signal received: %s.%s from %s at %s", 
                interface_name ?: "unknown", signal_name ?: "unknown", 
                sender_name ?: "unknown", object_path ?: "unknown");
    
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
        log_error("Failed to forward signal %s.%s: %s", 
                  interface_name ?: "unknown", signal_name ?: "unknown",
                  error ? error->message : "Unknown error");
        if (error) g_error_free(error);
    } else {
        log_verbose("Signal forwarded successfully");
    }
}

// Add specific NetworkManager state monitoring
static void setup_nm_state_monitoring()
{
    // Monitor NetworkManager state changes specifically
    guint nm_state_subscription = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        "org.freedesktop.NetworkManager",
        "StateChanged",
        "/org/freedesktop/NetworkManager",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *connection G_GNUC_UNUSED,
           const char *sender_name G_GNUC_UNUSED,
           const char *object_path,
           const char *interface_name,
           const char *signal_name,
           GVariant *parameters,
           gpointer user_data G_GNUC_UNUSED) {
            
            log_info("NetworkManager StateChanged signal received");
            
            // Extract state value for logging
            if (parameters) {
                guint32 state;
                if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)"))) {
                    g_variant_get(parameters, "(u)", &state);
                    log_info("NetworkManager state: %u", state);
                }
            }
            
            GError *error = NULL;
            g_dbus_connection_emit_signal(
                proxy_state->target_bus,
                NULL,
                object_path,
                interface_name,
                signal_name,
                parameters,
                &error);
                
            if (error) {
                log_error("Failed to forward StateChanged: %s", error->message);
                g_error_free(error);
            } else {
                log_info("StateChanged signal forwarded");
            }
        },
        NULL, NULL);
    
    if (nm_state_subscription) {
        g_hash_table_insert(proxy_state->signal_subscriptions,
                           GUINT_TO_POINTER(nm_state_subscription),
                           g_strdup("NetworkManager.StateChanged"));
    }
    
    // Monitor device state changes
    guint device_state_subscription = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        proxy_state->config.source_bus_name,
        "org.freedesktop.NetworkManager.Device",
        "StateChanged",
        NULL, // Any device path
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_signal_received_catchall_fixed, // Use the fixed handler
        NULL, NULL);
    
    if (device_state_subscription) {
        g_hash_table_insert(proxy_state->signal_subscriptions,
                           GUINT_TO_POINTER(device_state_subscription),
                           g_strdup("Device.StateChanged"));
    }
}

#if 0 // jarekk
// Forward signals from source bus to target bus - catch-all version
static void on_signal_received_catchall(GDBusConnection *connection G_GNUC_UNUSED,
                                        const char *sender_name,
                                        const char *object_path,
                                        const char *interface_name,
                                        const char *signal_name,
                                        GVariant *parameters,
                                        gpointer user_data G_GNUC_UNUSED)
{
    // Forward signals from our source service OR from the D-Bus daemon
    if (g_strcmp0(sender_name, proxy_state->config.source_bus_name) != 0 &&
        g_strcmp0(sender_name, "org.freedesktop.DBus") != 0) {
        return;
    }
    
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
        } else {
            log_verbose("Signal forwarded successfully");
        }
    }
}
#endif

// Handle properties changed signals specially
static void on_properties_changed(G_GNUC_UNUSED GDBusConnection *connection,
                                  const char *sender_name,
                                  const char *object_path,
                                  const char *interface_name,
                                  const char *signal_name,
                                  GVariant *parameters,
                                  G_GNUC_UNUSED gpointer user_data)
{
    // Only forward signals from our specific source
    if (g_strcmp0(sender_name, proxy_state->config.source_bus_name) != 0) {
        return;
    }
    
    // Only forward signals from our specific object path (or child paths)
    if (!g_str_has_prefix(object_path, proxy_state->config.source_object_path)) {
        return;
    }
    
    const char *changed_interface;
    g_variant_get_child(parameters, 0, "&s", &changed_interface);
    
    log_verbose("Properties changed signal for interface: %s at %s", changed_interface, object_path);
    
    // Forward the PropertiesChanged signal
    GError *error = NULL;
    gboolean success = g_dbus_connection_emit_signal(
        proxy_state->target_bus,
        NULL, // Broadcast to all subscribers
        object_path, // Use the original object path
        interface_name,
        signal_name,
        parameters,
        &error);
    
    if (success) {
        log_verbose("PropertiesChanged signal forwarded successfully");
    } else {
        log_error("Failed to forward PropertiesChanged signal: %s", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
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

static void emit_names_changed_signal()
{
    // Emit NameOwnerChanged signal to announce our service
    GError *error = NULL;
    gboolean success = g_dbus_connection_emit_signal(
        proxy_state->target_bus,
        NULL,  // broadcast to all
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        g_variant_new("(sss)", 
                     proxy_state->config.proxy_bus_name,  // service name
                     "",  // old owner (empty = new service)
                     g_dbus_connection_get_unique_name(proxy_state->target_bus)), // new owner
        &error);
    
    if (!success) {
        log_error("Failed to emit NameOwnerChanged: %s", error ? error->message : "Unknown");
        if (error) g_error_free(error);
    } else {
        log_info("Emitted NameOwnerChanged signal for service announcement");
    }
}

static GDBusMessage *
on_filter (GDBusConnection *connection G_GNUC_UNUSED,
           GDBusMessage    *message,
           gboolean         incoming G_GNUC_UNUSED,
           gpointer         user_data G_GNUC_UNUSED)
{
    if (g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL) {
        const gchar *iface = g_dbus_message_get_interface(message);
        const gchar *member = g_dbus_message_get_member(message);
        const gchar *path = g_dbus_message_get_path(message);
        const gchar *dest = g_dbus_message_get_destination(message);

        if (iface && g_strcmp0(iface, "org.freedesktop.DBus") == 0 &&
            member && g_strcmp0(member, "AddMatch") == 0 &&
            path && g_strcmp0(path, "/org/freedesktop/DBus") == 0 &&
            dest && g_strcmp0(dest, "org.freedesktop.DBus") == 0) {

            GVariant *body = g_dbus_message_get_body(message);
            if (body) {
                gchar *match_rule = NULL;
                g_variant_get(body, "(&s)", &match_rule);
                log_info("Caught AddMatch call: %s\n", match_rule);
            } else {
                log_info("Caught AddMatch call (no body)\n");
            }
        }
    }

    return message;
}

// Setup signal forwarding with both catch-all and specific PropertiesChanged handling
static gboolean setup_signal_forwarding()
{
    log_info("Setting up comprehensive signal forwarding");
    
    // Use the FIXED catch-all signal handler
    proxy_state->catch_all_subscription_id = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        NULL,                                // sender (catch ALL senders now!)
        NULL,                                // interface_name (all interfaces)
        NULL,                                // member (all signals)
        NULL,                                // object_path (all paths - we filter in callback)
        NULL,                                // arg0 (no filtering)
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_signal_received_catchall_fixed,   // Use the FIXED version
        NULL,
        NULL);
    
    if (proxy_state->catch_all_subscription_id == 0) {
        log_error("Failed to set up catch-all signal subscription");
        return FALSE;
    }

    log_info("Comprehensive signal subscription established on source bus (ID: %u)", 
             proxy_state->catch_all_subscription_id);
    
    // Add NetworkManager-specific monitoring
    setup_nm_state_monitoring();

     // Install filter to see all messages we are allowed to see
    g_dbus_connection_add_filter(proxy_state->target_bus,
                                 (GDBusMessageFilterFunction)on_filter,
                                 NULL, NULL);
    
    // Keep the existing PropertiesChanged subscription but make it broader
    guint props_subscription_id = g_dbus_connection_signal_subscribe(
        proxy_state->source_bus,
        NULL,  // ANY sender (not just our source service)
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        NULL, // All object paths
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *connection G_GNUC_UNUSED,
           const char *sender_name,
           const char *object_path,
           const char *interface_name,
           const char *signal_name,
           GVariant *parameters,
           gpointer user_data G_GNUC_UNUSED) {
            
            // Forward PropertiesChanged from NetworkManager paths or interfaces
            if ((object_path && g_str_has_prefix(object_path, "/org/freedesktop/NetworkManager")) ||
                (sender_name && g_str_has_prefix(sender_name, "org.freedesktop.NetworkManager"))) {
                
                log_verbose("Forwarding PropertiesChanged from %s at %s", sender_name, object_path);
                
                GError *error = NULL;
                g_dbus_connection_emit_signal(
                    proxy_state->target_bus,
                    NULL,
                    object_path,
                    interface_name,
                    signal_name,
                    parameters,
                    &error);
                    
                if (error) {
                    log_error("Failed to forward PropertiesChanged: %s", error->message);
                    g_error_free(error);
                }
            }
        },
        NULL, NULL);
    
    if (props_subscription_id == 0) {
        log_error("Failed to set up PropertiesChanged signal subscription");
        return FALSE;
    }

    // Signal to new clients appearing on the bus jarekk: doesn't work
    guint client_monitor = g_dbus_connection_signal_subscribe(
        proxy_state->target_bus,  // Monitor target bus
        "org.freedesktop.DBus", // Sender
        "org.freedesktop.DBus", // Interface
        "NameOwnerChanged", // Signal
        "/org/freedesktop/DBus", // Object path
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *connection G_GNUC_UNUSED,
        const char *sender_name G_GNUC_UNUSED,
        const char *object_path G_GNUC_UNUSED, 
        const char *interface_name G_GNUC_UNUSED,
        const char *signal_name G_GNUC_UNUSED,
        GVariant *parameters,
        gpointer user_data G_GNUC_UNUSED) {
            
            const char *service_name, *old_owner, *new_owner;
            g_variant_get(parameters, "(&s&s&s)", &service_name, &old_owner, &new_owner);
            
            // Detect when a new client appears (like nm-applet)
            if (strlen(new_owner) > 0 && strlen(old_owner) == 0) {
                log_info("New client connected: %s (owner: %s)", service_name, new_owner);
                
                // If NetworkManager proxy is already running, send state sync
                if (proxy_state && proxy_state->target_bus) {
                    // Send synthetic NameOwnerChanged for our service
                    emit_names_changed_signal();
                }
            }
        },
        NULL, NULL);

    if (client_monitor == 0) {
        log_error("Failed to set up NameOwnerChanged signal subscription");
        return FALSE;
    }

    setup_signal_debugging();

    g_hash_table_insert(proxy_state->signal_subscriptions,
                       GUINT_TO_POINTER(props_subscription_id),
                       g_strdup("Enhanced.PropertiesChanged"));
    
    log_info("Enhanced PropertiesChanged signal subscription established (ID: %u)", props_subscription_id);
    return TRUE;

    g_hash_table_insert(proxy_state->signal_subscriptions,
                       GUINT_TO_POINTER(client_monitor),
                       g_strdup("NameOwnerChanged"));
    
    log_info("NameOwnerChanged signal subscription established (ID: %u)", props_subscription_id);
    return TRUE;
}

// Register interfaces
static gboolean setup_proxy_interfaces()
{
    log_info("Setting up proxy interfaces - discovering full object tree");

#if 1 // wrong approach    
    // First, proxy the D-Bus daemon interface that clients use for service discovery
    if (!discover_and_proxy_object_tree("/org/freedesktop")) {
        log_error("Failed to discover and proxy D-Bus daemon interface");
        return FALSE;
    }
#endif

    // Start recursive discovery from the root object
    if (!discover_and_proxy_object_tree(proxy_state->config.source_object_path)) {
        log_error("Failed to discover and proxy object tree");
        return FALSE;
    }
    // jarekk is it needed?
    // // Also proxy the StatusNotifierItem so Cosmic sees nm-applet’s icon
    // if (!discover_and_proxy_object_tree("/StatusNotifierItem");)) {
    //     log_error("Failed to discover and proxy /StatusNotifierItem tree");
    //     return FALSE;
    // }

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
    
    // Give a small delay for all interfaces to be registered
    g_timeout_add(500, [](gpointer data G_GNUC_UNUSED) -> gboolean {
        emit_names_changed_signal();
        return FALSE; // one-shot timer
    }, NULL);
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
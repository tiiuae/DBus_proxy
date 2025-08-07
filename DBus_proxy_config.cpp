/*
 * Enhanced GDBus proxy with configuration file support:
 * 1. Connects to system bus.
 * 2. Reads configuration from file.
 * 3. Fetches introspection data from source service.
 * 4. Exposes that interface on its own name.
 * 5. Forwards method calls and signals to the original service.
 */

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>

// Configuration structure
struct ProxyConfig {
    std::string source_bus_name = "org.freedesktop.NetworkManager";
    std::string source_object_path = "/org/freedesktop/NetworkManager";
    std::string proxy_bus_name = "org.example.Proxy";
    std::string bus_type = "system";  // "system" or "session"
    bool verbose = false;
    bool enable_logging = true;
    int timeout_ms = 30000;
    std::string log_file = "";
    
    bool loadFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            g_printerr("Warning: Cannot open config file: %s, using defaults\n", filename.c_str());
            return false;
        }
        
        std::string line;
        int lineNum = 0;
        
        while (std::getline(file, line)) {
            lineNum++;
            
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            
            // Find the = separator
            size_t pos = line.find('=');
            if (pos == std::string::npos) {
                g_printerr("Warning: Invalid line %d in config: %s\n", lineNum, line.c_str());
                continue;
            }
            
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            // Remove quotes from value if present
            if (value.size() >= 2 && 
                ((value[0] == '"' && value.back() == '"') ||
                 (value[0] == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }
            
            // Set configuration values
            if (key == "source_bus_name") {
                source_bus_name = value;
            } else if (key == "source_object_path") {
                source_object_path = value;
            } else if (key == "proxy_bus_name") {
                proxy_bus_name = value;
            } else if (key == "bus_type") {
                bus_type = value;
            } else if (key == "verbose") {
                verbose = (value == "true" || value == "1" || value == "yes" || value == "on");
            } else if (key == "enable_logging") {
                enable_logging = (value == "true" || value == "1" || value == "yes" || value == "on");
            } else if (key == "timeout_ms") {
                timeout_ms = std::stoi(value);
            } else if (key == "log_file") {
                log_file = value;
            } else {
                g_printerr("Warning: Unknown config option: %s\n", key.c_str());
            }
        }
        
        file.close();
        return true;
    }
    
    void print() const {
        g_print("Configuration:\n");
        g_print("  source_bus_name: %s\n", source_bus_name.c_str());
        g_print("  source_object_path: %s\n", source_object_path.c_str());
        g_print("  proxy_bus_name: %s\n", proxy_bus_name.c_str());
        g_print("  bus_type: %s\n", bus_type.c_str());
        g_print("  verbose: %s\n", verbose ? "true" : "false");
        g_print("  enable_logging: %s\n", enable_logging ? "true" : "false");
        g_print("  timeout_ms: %d\n", timeout_ms);
        g_print("  log_file: %s\n", log_file.empty() ? "(none)" : log_file.c_str());
    }
    
    bool validate() const {
        if (source_bus_name.empty()) {
            g_printerr("Error: source_bus_name cannot be empty\n");
            return false;
        }
        if (source_object_path.empty()) {
            g_printerr("Error: source_object_path cannot be empty\n");
            return false;
        }
        if (proxy_bus_name.empty()) {
            g_printerr("Error: proxy_bus_name cannot be empty\n");
            return false;
        }
        if (bus_type != "system" && bus_type != "session") {
            g_printerr("Error: bus_type must be 'system' or 'session'\n");
            return false;
        }
        if (timeout_ms <= 0) {
            g_printerr("Error: timeout_ms must be positive\n");
            return false;
        }
        return true;
    }
    
    void saveTemplate(const std::string& filename) const {
        std::ofstream file(filename);
        if (!file.is_open()) {
            g_printerr("Error: Cannot create config template: %s\n", filename.c_str());
            return;
        }
        
        file << "# GDBus Proxy Configuration File\n";
        file << "# Lines starting with # or ; are comments\n\n";
        
        file << "# Source service to proxy\n";
        file << "source_bus_name=" << source_bus_name << "\n";
        file << "source_object_path=" << source_object_path << "\n\n";
        
        file << "# Proxy service name\n";
        file << "proxy_bus_name=" << proxy_bus_name << "\n\n";
        
        file << "# Bus type: 'system' or 'session'\n";
        file << "bus_type=" << bus_type << "\n\n";
        
        file << "# Enable verbose output\n";
        file << "verbose=" << (verbose ? "true" : "false") << "\n\n";
        
        file << "# Enable logging\n";
        file << "enable_logging=" << (enable_logging ? "true" : "false") << "\n\n";
        
        file << "# Timeout in milliseconds\n";
        file << "timeout_ms=" << timeout_ms << "\n\n";
        
        file << "# Log file (empty for stdout)\n";
        file << "log_file=" << log_file << "\n";
        
        file.close();
        g_print("Configuration template saved to: %s\n", filename.c_str());
    }
};

static ProxyConfig config;
static GDBusNodeInfo *introspection_data = NULL;
static GDBusConnection *bus = NULL;
static FILE *log_fp = NULL;

// Logging function
static void log_message(const char* format, ...) {
    if (!config.enable_logging) return;
    
    va_list args;
    va_start(args, format);
    
    if (log_fp) {
        vfprintf(log_fp, format, args);
        fflush(log_fp);
    } else {
        vprintf(format, args);
    }
    
    va_end(args);
}

// Forward method calls to the source service
static void handle_method_call(GDBusConnection *conn,
                               const char *sender,
                               const char *object_path,
                               const char *interface_name,
                               const char *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data)
{
    if (config.verbose) {
        log_message("Forwarding method call: %s.%s from %s\n", 
                   interface_name, method_name, sender);
    }
    
    g_dbus_connection_call(
        bus,                                    // Same bus
        config.source_bus_name.c_str(),       // Destination
        config.source_object_path.c_str(),    // Object path
        interface_name,                        // Interface
        method_name,                          // Method
        parameters,                           // Parameters
        NULL,                                 // Expected reply type
        G_DBUS_CALL_FLAGS_NONE,
        config.timeout_ms, NULL,              // Timeout, cancellable
        (GAsyncReadyCallback)[](GObject *source, GAsyncResult *res, gpointer user_data) {
            GDBusMethodInvocation *inv = (GDBusMethodInvocation *)user_data;
            GError *error = NULL;
            GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
            
            if (result) {
                if (config.verbose) {
                    log_message("Method call succeeded, returning result\n");
                }
                g_dbus_method_invocation_return_value(inv, result);
            } else {
                if (config.verbose) {
                    log_message("Method call failed: %s\n", error ? error->message : "Unknown error");
                }
                g_dbus_method_invocation_return_error_literal(inv, G_IO_ERROR, G_IO_ERROR_FAILED, 
                    error ? error->message : "Call failed");
                if (error) g_error_free(error);
            }
        },
        invocation);
}

// Forward property get requests to the source service
static GVariant* handle_get_property(GDBusConnection *conn,
                                    const char *sender,
                                    const char *object_path,
                                    const char *interface_name,
                                    const char *property_name,
                                    GError **error,
                                    gpointer user_data)
{
    if (config.verbose) {
        log_message("Getting property: %s.%s from %s\n", 
                   interface_name, property_name, sender);
    }
    
    GVariant *result = g_dbus_connection_call_sync(
        bus,
        config.source_bus_name.c_str(),
        config.source_object_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", interface_name, property_name),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        config.timeout_ms,
        NULL,
        error);
    
    if (!result) {
        if (config.verbose) {
            log_message("Failed to get property %s.%s: %s\n", 
                       interface_name, property_name, 
                       *error ? (*error)->message : "Unknown error");
        }
        return NULL;
    }
    
    GVariant *value;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    
    if (config.verbose) {
        log_message("Property %s.%s retrieved successfully\n", interface_name, property_name);
    }
    
    return value;
}

// Forward property set requests to the source service
static gboolean handle_set_property(GDBusConnection *conn,
                                   const char *sender,
                                   const char *object_path,
                                   const char *interface_name,
                                   const char *property_name,
                                   GVariant *value,
                                   GError **error,
                                   gpointer user_data)
{
    if (config.verbose) {
        log_message("Setting property: %s.%s from %s\n", 
                   interface_name, property_name, sender);
    }
    
    GVariant *result = g_dbus_connection_call_sync(
        bus,
        config.source_bus_name.c_str(),
        config.source_object_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", interface_name, property_name, value),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        config.timeout_ms,
        NULL,
        error);
    
    if (!result) {
        if (config.verbose) {
            log_message("Failed to set property %s.%s: %s\n", 
                       interface_name, property_name, 
                       *error ? (*error)->message : "Unknown error");
        }
        return FALSE;
    }
    
    g_variant_unref(result);
    
    if (config.verbose) {
        log_message("Property %s.%s set successfully\n", interface_name, property_name);
    }
    
    return TRUE;
}

// Forward signals from the source to the proxy (including property changes)
static void on_signal_received(GDBusConnection *conn,
                               const char *sender_name,
                               const char *object_path,
                               const char *interface_name,
                               const char *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
    if (config.verbose) {
        log_message("Forwarding signal: %s.%s\n", interface_name, signal_name);
    }
    
    GError *error = NULL;
    gboolean success = g_dbus_connection_emit_signal(
        bus,
        NULL,                                  // destination (broadcast)
        config.source_object_path.c_str(),    // object path
        interface_name,
        signal_name,
        parameters,
        &error);
    
    if (!success && config.verbose) {
        log_message("Failed to emit signal: %s\n", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
    }
}

// Subscribe to PropertiesChanged signals for all interfaces
static void subscribe_to_properties_changed()
{
    if (config.verbose) {
        log_message("Subscribing to PropertiesChanged signals\n");
    }
    
    guint subscription_id = g_dbus_connection_signal_subscribe(
        bus,
        config.source_bus_name.c_str(),
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        config.source_object_path.c_str(),
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_signal_received,
        NULL,
        NULL);
        
    if (subscription_id == 0) {
        g_printerr("Failed to subscribe to PropertiesChanged signals\n");
    } else if (config.verbose) {
        log_message("Subscribed to PropertiesChanged signals (subscription ID: %u)\n", subscription_id);
    }
}

static void print_usage(const char* program_name) {
    g_print("Usage: %s [OPTIONS]\n", program_name);
    g_print("Options:\n");
    g_print("  -c, --config FILE     Use specific config file (default: proxy.conf)\n");
    g_print("  -h, --help            Show this help\n");
    g_print("  --create-config FILE  Create a sample config file\n");
    g_print("  --show-config         Show current configuration and exit\n");
    g_print("\nConfig file format:\n");
    g_print("  source_bus_name=org.freedesktop.NetworkManager\n");
    g_print("  source_object_path=/org/freedesktop/NetworkManager\n");
    g_print("  proxy_bus_name=org.example.Proxy\n");
    g_print("  bus_type=system\n");
    g_print("  verbose=false\n");
    g_print("  enable_logging=true\n");
    g_print("  timeout_ms=30000\n");
    g_print("  log_file=\n");
}

int main(int argc, char* argv[])
{
    std::string config_file = "proxy.conf";
    bool show_config_only = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[i + 1];
                i++; // Skip next argument
            } else {
                g_printerr("Error: --config requires a filename\n");
                return 1;
            }
        } else if (arg == "--create-config") {
            if (i + 1 < argc) {
                config.saveTemplate(argv[i + 1]);
                return 0;
            } else {
                g_printerr("Error: --create-config requires a filename\n");
                return 1;
            }
        } else if (arg == "--show-config") {
            show_config_only = true;
        } else {
            g_printerr("Error: Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Load configuration
    g_print("Loading configuration from: %s\n", config_file.c_str());
    config.loadFromFile(config_file);
    
    // Validate configuration
    if (!config.validate()) {
        return 1;
    }
    
    if (show_config_only) {
        config.print();
        return 0;
    }
    
    if (config.verbose) {
        config.print();
    }
    
    // Open log file if specified
    if (!config.log_file.empty()) {
        log_fp = fopen(config.log_file.c_str(), "a");
        if (!log_fp) {
            g_printerr("Warning: Cannot open log file: %s\n", config.log_file.c_str());
        }
    }
    
    // Connect to bus
    GError *error = NULL;
    GBusType bus_type = (config.bus_type == "system") ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION;
    
    bus = g_bus_get_sync(bus_type, NULL, &error);
    if (!bus) {
        g_printerr("Failed to connect to %s bus: %s\n", config.bus_type.c_str(), error->message);
        g_error_free(error);
        return 1;
    }
    
    log_message("Connected to %s bus\n", config.bus_type.c_str());
    
    // Introspect source object
    log_message("Introspecting %s at %s\n", config.source_bus_name.c_str(), config.source_object_path.c_str());
    
    GVariant *xml = g_dbus_connection_call_sync(
        bus,
        config.source_bus_name.c_str(),
        config.source_object_path.c_str(),
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        config.timeout_ms,
        NULL,
        &error);

    if (!xml) {
        g_printerr("Introspection failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    const char *xml_data;
    g_variant_get(xml, "(s)", &xml_data);
    introspection_data = g_dbus_node_info_new_for_xml(xml_data, &error);
    if (!introspection_data) {
        g_printerr("Failed to parse introspection XML: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    
    g_variant_unref(xml);
    log_message("Introspection successful\n");

    // Register interfaces and subscribe to signals
    GDBusInterfaceVTable vtable = {
        .method_call = handle_method_call,
        .get_property = handle_get_property,
        .set_property = handle_set_property
    };

    int interface_count = 0;
    int signal_count = 0;
    
    for (int i = 0; introspection_data->interfaces[i]; i++) {
        GDBusInterfaceInfo *iface = introspection_data->interfaces[i];
        
        log_message("Registering interface: %s\n", iface->name);
        
        guint registration_id = g_dbus_connection_register_object(
            bus,
            config.source_object_path.c_str(),
            iface,
            &vtable,
            NULL, NULL, &error);
            
        if (registration_id == 0) {
            g_printerr("Failed to register interface %s: %s\n", iface->name, error->message);
            g_error_free(error);
            continue;
        }
        
        interface_count++;

        // Subscribe to signals for this interface
        for (int j = 0; iface->signals && iface->signals[j]; j++) {
            if (config.verbose) {
                log_message("Subscribing to signal: %s.%s\n", iface->name, iface->signals[j]->name);
            }
            
            guint subscription_id = g_dbus_connection_signal_subscribe(
                bus,
                config.source_bus_name.c_str(),
                iface->name,
                iface->signals[j]->name,
                config.source_object_path.c_str(),
                NULL,
                G_DBUS_SIGNAL_FLAGS_NONE,
                on_signal_received,
                NULL,
                NULL);
                
            if (subscription_id == 0) {
                g_printerr("Failed to subscribe to signal %s.%s\n", iface->name, iface->signals[j]->name);
            } else {
                signal_count++;
            }
        }
    }
    
    log_message("Registered %d interfaces with %d signal subscriptions\n", interface_count, signal_count);
    
    // Subscribe to PropertiesChanged signals for the entire object
    subscribe_to_properties_changed();

    // Acquire a name
    log_message("Acquiring bus name: %s\n", config.proxy_bus_name.c_str());
    
    g_bus_own_name(bus_type, config.proxy_bus_name.c_str(),
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   NULL, NULL, NULL, NULL, NULL);

    g_print("Proxy running: %s -> %s on %s bus\n", 
            config.source_bus_name.c_str(), 
            config.proxy_bus_name.c_str(),
            config.bus_type.c_str());
    
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    
    // Cleanup
    if (log_fp) {
        fclose(log_fp);
    }
    
    return 0;
}
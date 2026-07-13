#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>

// Estructura global para manejar el estado, la red y los widgets de la app
typedef struct {
    GtkWidget *stack;
    GtkWidget *entrada_usuario;
    GtkWidget *lista_mensajes;
    GtkWidget *entrada_mensaje;
    GtkWidget *scroll_ventana;
    gchar *nombre_usuario;
    gchar *ruta_config;
    GSocketConnection *conexion;
} TuxContexto;

// Prototipos de funciones
static void cargar_configuracion(TuxContexto *ctx);
static void guardar_configuracion(TuxContexto *ctx, const gchar *nombre);
static void al_registrar_usuario(GtkButton *boton, gpointer data);
static void al_enviar_mensaje(GtkButton *boton, gpointer data);
static void construir_interfaz_chat(TuxContexto *ctx, GtkWidget *contenedor);
static void agregar_mensaje_a_pantalla(TuxContexto *ctx, const gchar *texto_formateado);
static gpointer hilo_escucha_red(gpointer data);
static gboolean agregar_mensaje_desde_red(gpointer data);

// Retorna la ruta del archivo de configuración (~/.config/tux_config.ini)
static gchar *obtener_ruta_config() {
    const gchar *config_dir = g_get_user_config_dir();
    return g_build_filename(config_dir, "tux_config.ini", NULL);
}

static void cargar_configuracion(TuxContexto *ctx) {
    GKeyFile *keyfile = g_key_file_new();
    ctx->ruta_config = obtener_ruta_config();
    
    if (g_key_file_load_from_file(keyfile, ctx->ruta_config, G_KEY_FILE_NONE, NULL)) {
        ctx->nombre_usuario = g_key_file_get_string(keyfile, "Perfil", "Usuario", NULL);
    } else {
        ctx->nombre_usuario = NULL;
    }
    g_key_file_free(keyfile);
}

static void guardar_configuracion(TuxContexto *ctx, const gchar *nombre) {
    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_string(keyfile, "Perfil", "Usuario", nombre);
    g_key_file_save_to_file(keyfile, ctx->ruta_config, NULL);
    ctx->nombre_usuario = g_strdup(nombre);
    g_key_file_free(keyfile);
}

// Conectar al servidor centralizado
static void conectar_al_servidor(TuxContexto *ctx) {
    GSocketClient *client = g_socket_client_new();
    GError *error = NULL;

    // Conectar a Localhost (127.0.0.1) en el puerto 12345
    ctx->conexion = g_socket_client_connect_to_host(client, "127.0.0.1", 12345, NULL, &error);
    
    if (error != NULL) {
        g_printerr("No se pudo conectar al servidor de Tux Messenger: %s\n", error->message);
        agregar_mensaje_a_pantalla(ctx, "<i>Sistema: No se pudo conectar al servidor. Modo sin conexión.</i>");
        g_error_free(error);
    } else {
        g_print("Conectado exitosamente al servidor de Tux Messenger.\n");
        // Crear un hilo secundario para escuchar mensajes entrantes sin bloquear GTK
        g_thread_new("hilo_red_tux", hilo_escucha_red, ctx);
    }
    g_object_unref(client);
}

// Evento: El usuario hace clic en el botón de registro
static void al_registrar_usuario(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    GtkEntryBuffer *buffer = gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_usuario));
    const char *nombre = gtk_entry_buffer_get_text(buffer);

    if (g_strcmp0(nombre, "") == 0) return;

    guardar_configuracion(ctx, nombre);
    
    // Cambiar a la pantalla de chat e intentar conectar
    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "pantalla_chat");
    conectar_al_servidor(ctx);
}

// Helper para renderizar los mensajes en el GtkListBox
static void agregar_mensaje_a_pantalla(TuxContexto *ctx, const gchar *texto_formateado) {
    GtkWidget *fila = gtk_list_box_row_new();
    GtkWidget *etiqueta = gtk_label_new(NULL);
    
    gtk_label_set_markup(GTK_LABEL(etiqueta), texto_formateado);
    gtk_widget_set_halign(etiqueta, GTK_ALIGN_START);
    gtk_widget_set_margin_start(etiqueta, 10);
    gtk_widget_set_margin_top(etiqueta, 5);
    gtk_widget_set_margin_bottom(etiqueta, 5);
    
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(fila), etiqueta);
    gtk_list_box_append(GTK_LIST_BOX(ctx->lista_mensajes), fila);

    // Auto-scroll hacia el final para ver el nuevo mensaje
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ctx->scroll_ventana));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

// Evento: El usuario envía un mensaje
static void al_enviar_mensaje(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    GtkEntryBuffer *buffer = gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_mensaje));
    const char *texto = gtk_entry_buffer_get_text(buffer);

    if (g_strcmp0(texto, "") == 0) return;

    // Formatear mensaje con el nombre del usuario emisor
    gchar *mensaje_formateado = g_strdup_printf("<b>%s</b>: %s", ctx->nombre_usuario, texto);

    // 1. Mostrarlo localmente de inmediato
    agregar_mensaje_a_pantalla(ctx, mensaje_formateado);

    // 2. Enviarlo al servidor para que lo distribuya al resto (Broadcasting)
    if (ctx->conexion) {
        GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conexion));
        // Agregamos un salto de línea al final para delimitar los mensajes en la red
        gchar *mensaje_red = g_strdup_printf("%s\n", mensaje_formateado);
        
        g_output_stream_write(output, mensaje_red, strlen(mensaje_red), NULL, NULL);
        g_free(mensaje_red);
    }

    // Limpiar barra de entrada de texto
    gtk_entry_buffer_set_text(buffer, "", 0);
    g_free(mensaje_formateado);
}

// Estructura auxiliar para pasar texto entre hilos de forma segura
typedef struct {
    TuxContexto *ctx;
    gchar *mensaje;
} RedMensajeData;

// Se ejecuta en el Hilo Principal (UI Thread) invocado por g_idle_add
static gboolean agregar_mensaje_desde_red(gpointer data) {
    RedMensajeData *msg_data = (RedMensajeData *)data;
    
    // Pintar en pantalla el mensaje recibido de otro usuario
    agregar_mensaje_a_pantalla(msg_data->ctx, msg_data->mensaje);
    
    g_free(msg_data->mensaje);
    g_free(msg_data);
    return G_SOURCE_REMOVE; // Quitar del loop de eventos
}

// Bucle en segundo plano que escucha los sockets sin congelar la ventana gráfica
static gpointer hilo_escucha_red(gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(ctx->conexion));
    
    // DataInputStream nos ayuda a leer línea por línea de forma sencilla (\n)
    GDataInputStream *data_input = g_data_input_stream_new(input);
    
    while (1) {
        gsize longitud;
        GError *error = NULL;
        // Leer hasta encontrar el salto de línea que envía el servidor
        gchar *linea = g_data_input_stream_read_line(data_input, &longitud, NULL, &error);
        
        if (linea == NULL || error != NULL) {
            if (error) g_error_free(error);
            break; // Conexión cerrada o error
        }

        // Crear contenedor para enviar los datos de vuelta de forma segura al hilo principal
        RedMensajeData *msg_data = g_new0(RedMensajeData, 1);
        msg_data->ctx = ctx;
        msg_data->mensaje = linea; // Asignamos la cadena leída de la red

        // IMPORTANTE: En GTK no puedes modificar la interfaz desde otro hilo. 
        // Usamos g_idle_add para programar la actualización de forma segura.
        g_idle_add(agregar_mensaje_desde_red, msg_data);
    }

    g_object_unref(data_input);
    g_print("El hilo de escucha de red ha finalizado.\n");
    return NULL;
}

static void construir_interfaz_chat(TuxContexto *ctx, GtkWidget *contenedor) {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 180);
    gtk_box_append(GTK_BOX(contenedor), paned);

    // --- BARRA LATERAL ---
    GtkWidget *barra_lateral = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(barra_lateral, 150, -1);
    
    GtkWidget *lbl_contactos = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_contactos), "<span weight='bold'>Salas de Chat</span>");
    gtk_box_append(GTK_BOX(barra_lateral), lbl_contactos);

    GtkWidget *lista_canales = gtk_list_box_new();
    GtkWidget *canal_general = gtk_label_new("# general (Global)");
    gtk_widget_set_halign(canal_general, GTK_ALIGN_START);
    gtk_list_box_append(GTK_LIST_BOX(lista_canales), canal_general);
    gtk_box_append(GTK_BOX(barra_lateral), lista_canales);
    
    gtk_paned_set_start_child(GTK_PANED(paned), barra_lateral);

    // --- ÁREA DE MENSAJES ---
    GtkWidget *area_chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    
    ctx->scroll_ventana = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(ctx->scroll_ventana, TRUE);
    
    ctx->lista_mensajes = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ctx->lista_mensajes), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ctx->scroll_ventana), ctx->lista_mensajes);
    gtk_box_append(GTK_BOX(area_chat), ctx->scroll_ventana);

    // Barra de entrada inferior
    GtkWidget *barra_inferior = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    ctx->entrada_mensaje = gtk_entry_new();
    gtk_widget_set_hexpand(ctx->entrada_mensaje, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_mensaje), "Escribe un mensaje para todos...");
    
    g_signal_connect_swapped(ctx->entrada_mensaje, "activate", G_CALLBACK(al_enviar_mensaje), ctx);

    GtkWidget *btn_enviar = gtk_button_new_with_label("Enviar");
    g_signal_connect(btn_enviar, "clicked", G_CALLBACK(al_enviar_mensaje), ctx);

    gtk_box_append(GTK_BOX(barra_inferior), ctx->entrada_mensaje);
    gtk_box_append(GTK_BOX(barra_inferior), btn_enviar);
    gtk_box_append(GTK_BOX(area_chat), barra_inferior);

    gtk_paned_set_end_child(GTK_PANED(paned), area_chat);
}

static void activar_app(GtkApplication *app, gpointer user_data) {
    TuxContexto *ctx = (TuxContexto *)user_data;

    GtkWidget *ventana = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ventana), "Tux Messenger");
    gtk_window_set_default_size(GTK_WINDOW(ventana), 680, 480);

    ctx->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ctx->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_window_set_child(GTK_WINDOW(ventana), ctx->stack);

    // 1. PANTALLA DE REGISTRO
    GtkWidget *vista_registro = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_start(vista_registro, 60);
    gtk_widget_set_margin_end(vista_registro, 60);
    gtk_widget_set_valign(vista_registro, GTK_ALIGN_CENTER);

    GtkWidget *titulo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(titulo), "<span size='xx-large' weight='bold'>Tux Messenger</span>");
    gtk_box_append(GTK_BOX(vista_registro), titulo);

    ctx->entrada_usuario = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_usuario), "Nombre de usuario...");
    gtk_box_append(GTK_BOX(vista_registro), ctx->entrada_usuario);

    GtkWidget *btn_conectar = gtk_button_new_with_label("Entrar a Tux Messenger");
    g_signal_connect(btn_conectar, "clicked", G_CALLBACK(al_registrar_usuario), ctx);
    gtk_box_append(GTK_BOX(vista_registro), btn_conectar);

    // 2. PANTALLA DE CHAT PRINCIPAL
    GtkWidget *vista_chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    construir_interfaz_chat(ctx, vista_chat);

    gtk_stack_add_named(GTK_STACK(ctx->stack), vista_registro, "pantalla_registro");
    gtk_stack_add_named(GTK_STACK(ctx->stack), vista_chat, "pantalla_chat");

    if (ctx->nombre_usuario != NULL) {
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "pantalla_chat");
        conectar_al_servidor(ctx);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "pantalla_registro");
    }

    gtk_window_present(GTK_WINDOW(ventana));
}

int main(int argc, char **argv) {
    GtkApplication *app;
    int estado;

    TuxContexto *ctx = g_new0(TuxContexto, 1);
    cargar_configuracion(ctx);

    app = gtk_application_new("org.tuxmessenger.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activar_app), ctx);
    
    estado = g_application_run(G_APPLICATION(app), argc, argv);
    
    // Limpieza al cerrar la aplicación
    if (ctx->conexion) {
        g_io_stream_close(G_IO_STREAM(ctx->conexion), NULL, NULL);
        g_object_unref(ctx->conexion);
    }
    g_object_unref(app);
    g_free(ctx->nombre_usuario);
    g_free(ctx->ruta_config);
    g_free(ctx);

    return estado;
}

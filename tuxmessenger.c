#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>

// Estructura global para manejar el estado, la red y los widgets de la app
typedef struct {
    GtkWidget *stack;
    GtkWidget *entrada_usuario;
    GtkWidget *lista_mensajes;
    GtkWidget *entrada_mensaje;
    GtkWidget *entrada_dm_destino;
    GtkWidget *entrada_reporte_user;
    GtkWidget *entrada_reporte_motivo;
    GtkWidget *scroll_ventana;
    GtkWidget *lbl_error_registro;
    gchar *nombre_usuario;
    gchar *ruta_config;
    GSocketConnection *conexion;
} TuxContexto;

// Estructura auxiliar para pasar texto entre hilos de forma segura
typedef struct {
    TuxContexto *ctx;
    gchar *mensaje;
} RedMensajeData;

// Prototipos de funciones estrictamente declarados y cerrados
static void al_enviar_mensaje(GtkButton *boton, gpointer data);
static void construir_interfaz_chat(TuxContexto *ctx, GtkWidget *contenedor);
static void conectar_al_servidor(TuxContexto *ctx);
static gpointer hilo_escucha_red(gpointer data);

// Retorna la ruta del archivo de configuración (~/.config/tux_config.ini)
static gchar *obtener_ruta_config() {
    return g_build_filename(g_get_user_config_dir(), "tux_config.ini", NULL);
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

// Helper para renderizar los mensajes en el GtkListBox con formato HTML
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

// Función intermedia (wrapper) para capturar el Enter del teclado sin crasheos
static void al_presionar_enter(GtkEntry *entrada, gpointer data) {
    al_enviar_mensaje(NULL, data);
}

// Evento: El usuario envía un mensaje global (#general)
static void al_enviar_mensaje(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    GtkEntryBuffer *buffer = gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_mensaje));
    const char *texto = gtk_entry_buffer_get_text(buffer);

    if (strcmp(texto, "") == 0) return;

    // Formatear mensaje localmente
    gchar *mensaje_formateado = g_strdup_printf("<b>%s</b>: %s", ctx->nombre_usuario, texto);
    agregar_mensaje_a_pantalla(ctx, mensaje_formateado);

    // Enviar por red al servidor si existe conexión activa
    if (ctx->conexion) {
        GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conexion));
        gchar *mensaje_red = g_strdup_printf("%s\n", mensaje_formateado);
        g_output_stream_write(output, mensaje_red, strlen(mensaje_red), NULL, NULL);
        g_free(mensaje_red);
    }
    
    gtk_entry_buffer_set_text(buffer, "", 0);
    g_free(mensaje_formateado);
}

// Evento: El usuario envía un Mensaje Directo (DM) privado
static void al_enviar_dm(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    const char *destino = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_dm_destino)));
    const char *texto = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_mensaje)));

    if (strcmp(destino, "") == 0 || strcmp(texto, "") == 0) return;

    if (ctx->conexion) {
        GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conexion));
        gchar *comando_dm = g_strdup_printf("DM:%s:%s\n", destino, texto);
        g_output_stream_write(output, comando_dm, strlen(comando_dm), NULL, NULL);
        g_free(comando_dm);

        // Mostrar en nuestra propia pantalla que enviamos el DM
        gchar *pantalla_dm = g_strdup_printf("<b>[DM para %s]</b>: %s", destino, texto);
        agregar_mensaje_a_pantalla(ctx, pantalla_dm);
        g_free(pantalla_dm);
    }
    
    // Limpiamos la barra de mensajes principal
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_mensaje)), "", 0);
}

// Evento: El usuario envía un reporte
static void al_enviar_reporte(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    const char *user = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_reporte_user)));
    const char *motivo = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_reporte_motivo)));

    if (strcmp(user, "") == 0 || strcmp(motivo, "") == 0) return;

    if (ctx->conexion) {
        GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conexion));
        gchar *comando_rep = g_strdup_printf("REPORTE:%s:%s\n", user, motivo);
        g_output_stream_write(output, comando_rep, strlen(comando_rep), NULL, NULL);
        g_free(comando_rep);
    }
    
    // Limpiar cajas de reportes
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_reporte_user)), "", 0);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_reporte_motivo)), "", 0);
}

// Se ejecuta de manera segura en el hilo principal de la UI
static gboolean agregar_mensaje_desde_red(gpointer data) {
    RedMensajeData *msg_data = (RedMensajeData *)data;
    agregar_mensaje_a_pantalla(msg_data->ctx, msg_data->mensaje);
    g_free(msg_data->mensaje);
    g_free(msg_data);
    return G_SOURCE_REMOVE;
}

// Muestra el error de nombre duplicado en la UI
static gboolean mostrar_error_usuario_ocupado(gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    gtk_label_set_markup(GTK_LABEL(ctx->lbl_error_registro), "<span color='red' weight='bold'>El nombre de usuario ya está ocupado. Prueba otro.</span>");
    if (ctx->conexion) {
        g_io_stream_close(G_IO_STREAM(ctx->conexion), NULL, NULL);
        ctx->conexion = NULL;
    }
    return G_SOURCE_REMOVE;
}

static gboolean transicionar_al_chat(gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "pantalla_chat");
    return G_SOURCE_REMOVE;
}

// Bucle secundario en segundo plano para procesar flujos de red
static gpointer hilo_escucha_red(gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(ctx->conexion));
    GDataInputStream *data_input = g_data_input_stream_new(input);
    gboolean verificado = FALSE;

    while (1) {
        gsize len;
        GError *err = NULL;
        gchar *linea = g_data_input_stream_read_line(data_input, &len, NULL, &err);
        if (linea == NULL || err != NULL) {
            if (err) g_error_free(err);
            break;
        }

        // CONTROL DE VALIDACIÓN AL ENTRAR
        if (!verificado) {
            if (strcmp(linea, "ERROR_USUARIO_OCUPADO") == 0) {
                g_idle_add(mostrar_error_usuario_ocupado, ctx);
                g_free(linea);
                break;
            } else if (strcmp(linea, "REGISTRO_OK") == 0) {
                verificado = TRUE;
                guardar_configuracion(ctx, ctx->nombre_usuario);
                g_idle_add(transicionar_al_chat, ctx);
                g_free(linea);
                continue;
            }
        }

        // MENSAJES NORMALES DEL SERVIDOR
        RedMensajeData *m_data = g_new0(RedMensajeData, 1);
        m_data->ctx = ctx;
        m_data->mensaje = linea;
        g_idle_add(agregar_mensaje_desde_red, m_data);
    }
    g_object_unref(data_input);
    return NULL;
}

static void al_registrar_usuario(GtkButton *boton, gpointer data) {
    TuxContexto *ctx = (TuxContexto *)data;
    const char *nombre = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entrada_usuario)));
    if (strcmp(nombre, "") == 0) return;

    ctx->nombre_usuario = g_strdup(nombre);
    conectar_al_servidor(ctx);
}

static void conectar_al_servidor(TuxContexto *ctx) {
    GSocketClient *client = g_socket_client_new();
    GError *error = NULL;
    ctx->conexion = g_socket_client_connect_to_host(client, "127.0.0.1", 12345, NULL, &error);
    g_object_unref(client);

    if (error != NULL) {
        gtk_label_set_text(GTK_LABEL(ctx->lbl_error_registro), "Servidor fuera de línea.");
        g_error_free(error);
        return;
    }

    g_thread_new("hilo_red", hilo_escucha_red, ctx);
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(ctx->conexion));
    gchar *solicitud = g_strdup_printf("REGISTRO:%s\n", ctx->nombre_usuario);
    g_output_stream_write(output, solicitud, strlen(solicitud), NULL, NULL);
    g_free(solicitud);
}

static void construir_interfaz_chat(TuxContexto *ctx, GtkWidget *contenedor) {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 180);
    gtk_box_append(GTK_BOX(contenedor), paned);

    // --- BARRA LATERAL ---
    GtkWidget *barra_lateral = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *lbl_c = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_c), "<b>Salas de Chat</b>");
    gtk_box_append(GTK_BOX(barra_lateral), lbl_c);

    GtkWidget *lista_canales = gtk_list_box_new();
    gtk_list_box_append(GTK_LIST_BOX(lista_canales), gtk_label_new("# general"));
    gtk_box_append(GTK_BOX(barra_lateral), lista_canales);
    gtk_paned_set_start_child(GTK_PANED(paned), barra_lateral);

    // --- ÁREA CENTRAL DE MENSAJES ---
    GtkWidget *area_chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    ctx->scroll_ventana = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(ctx->scroll_ventana, TRUE);
    ctx->lista_mensajes = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ctx->lista_mensajes), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ctx->scroll_ventana), ctx->lista_mensajes);
    gtk_box_append(GTK_BOX(area_chat), ctx->scroll_ventana);

    // Caja de Entrada de Mensaje Global
    GtkWidget *box_msg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    ctx->entrada_mensaje = gtk_entry_new();
    gtk_widget_set_hexpand(ctx->entrada_mensaje, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_mensaje), "Escribe un mensaje aquí...");
    g_signal_connect(ctx->entrada_mensaje, "activate", G_CALLBACK(al_presionar_enter), ctx);
    
    GtkWidget *btn_env = gtk_button_new_with_label("Enviar Global");
    g_signal_connect(btn_env, "clicked", G_CALLBACK(al_enviar_mensaje), ctx);
    gtk_box_append(GTK_BOX(box_msg), ctx->entrada_mensaje);
    gtk_box_append(GTK_BOX(box_msg), btn_env);
    gtk_box_append(GTK_BOX(area_chat), box_msg);

    // --- BARRA INFERIOR DE HERRAMIENTAS (DM y Reportes) ---
    GtkWidget *box_tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(box_tools, 5);
    gtk_widget_set_margin_bottom(box_tools, 5);
    
    // Controles para Mensajes Directos (DM)
    ctx->entrada_dm_destino = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_dm_destino), "Para (Usuario)...");
    GtkWidget *btn_dm = gtk_button_new_with_label("Enviar DM");
    g_signal_connect(btn_dm, "clicked", G_CALLBACK(al_enviar_dm), ctx);
    gtk_box_append(GTK_BOX(box_tools), ctx->entrada_dm_destino);
    gtk_box_append(GTK_BOX(box_tools), btn_dm);

    // Controles para Reportar Usuarios
    ctx->entrada_reporte_user = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_reporte_user), "Reportar a...");
    ctx->entrada_reporte_motivo = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_reporte_motivo), "Motivo del reporte...");
    GtkWidget *btn_rep = gtk_button_new_with_label("Reportar");
    g_signal_connect(btn_rep, "clicked", G_CALLBACK(al_enviar_reporte), ctx);
    gtk_box_append(GTK_BOX(box_tools), ctx->entrada_reporte_user);
    gtk_box_append(GTK_BOX(box_tools), ctx->entrada_reporte_motivo);
    gtk_box_append(GTK_BOX(box_tools), btn_rep);

    gtk_box_append(GTK_BOX(area_chat), box_tools);
    gtk_paned_set_end_child(GTK_PANED(paned), area_chat);
}

static void activar_app(GtkApplication *app, gpointer user_data) {
    TuxContexto *ctx = (TuxContexto *)user_data;
    GtkWidget *ventana = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ventana), "Tux Messenger");
    gtk_window_set_default_size(GTK_WINDOW(ventana), 850, 520);

    ctx->stack = gtk_stack_new();
    gtk_window_set_child(GTK_WINDOW(ventana), ctx->stack);

    // 1. PANTALLA DE REGISTRO
    GtkWidget *vista_reg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_start(vista_reg, 60);
    gtk_widget_set_margin_end(vista_reg, 60);
    gtk_widget_set_valign(vista_reg, GTK_ALIGN_CENTER);

    GtkWidget *titulo = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(titulo), "<span size='xx-large' weight='bold'>Tux Messenger</span>");
    gtk_box_append(GTK_BOX(vista_reg), titulo);

    ctx->entrada_usuario = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entrada_usuario), "Elige tu nombre de usuario...");
    ctx->lbl_error_registro = gtk_label_new("");
    
    GtkWidget *btn_con = gtk_button_new_with_label("Ingresar a la Red");
    g_signal_connect(btn_con, "clicked", G_CALLBACK(al_registrar_usuario), ctx);
    
    gtk_box_append(GTK_BOX(vista_reg), ctx->entrada_usuario);
    gtk_box_append(GTK_BOX(vista_reg), ctx->lbl_error_registro);
    gtk_box_append(GTK_BOX(vista_reg), btn_con);

    // 2. PANTALLA DE CHAT PRINCIPAL
    GtkWidget *vista_chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    construir_interfaz_chat(ctx, vista_chat);

    gtk_stack_add_named(GTK_STACK(ctx->stack), vista_reg, "pantalla_registro");
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
    TuxContexto *ctx = g_new0(TuxContexto, 1);
    cargar_configuracion(ctx);
    
    GtkApplication *app = gtk_application_new("org.tuxmessenger.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activar_app), ctx);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    
    // Recursos de limpieza al cerrar la app
    if (ctx->conexion) g_object_unref(ctx->conexion);
    g_object_unref(app);
    g_free(ctx->nombre_usuario);
    g_free(ctx->ruta_config);
    g_free(ctx);
    return status;
}

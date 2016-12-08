#include <iostream>
#include <glib.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <stdexcept>
#include <map>
#include <vector>
#include <mutex>
#include "scope_guard.hpp"


namespace
{


gboolean exit_sighandler(gpointer p_data)
{
	std::cerr << "caught signal, stopping mainloop\n";
	GMainLoop *mainloop = reinterpret_cast < GMainLoop* > (p_data);
	g_main_loop_quit(mainloop);
	return TRUE;
}




class http_stream_pipeline
{
public:
	explicit http_stream_pipeline(std::string p_content_type, char **pipeline_cmdline_argv)
		: m_pipeline(nullptr)
		, m_multisocketsink(nullptr)
		, m_content_type(std::move(p_content_type))
	{
		GError *gerror = nullptr;
		GstElement *cmdline_bin = nullptr, *stream_element = nullptr;

		// Scope guard to ensure elements are unref'd in case of an exception/error
		auto elements_guard = make_scope_guard([&stream_element, &cmdline_bin, this]()
		{
			// Using a vector here instead of an initializer list as a workaround
			// for a C++11 bug that was corrected in C++14. The bug was reported
			// as DR 1288 (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=50025).
			std::vector < GstElement ** > elements = { &stream_element, &cmdline_bin, &m_multisocketsink, &m_pipeline };

			// Unref all elements and make sure their pointers are set to null
			for (GstElement** elem : elements)
			{
				if (*elem != nullptr)
				{
					gst_object_unref(GST_OBJECT(*elem));
					*elem = nullptr;
				}
			}
		});


		// Parse the command line
		cmdline_bin = gst_parse_launchv((gchar const **)pipeline_cmdline_argv, &gerror);
		if (cmdline_bin == nullptr)
		{
			std::string s = std::string("could not parse pipeline: ") + gerror->message;
			g_clear_error(&gerror);
			throw std::runtime_error(s);
		}


		// Add a ghost srcpad to the bin and connect it to the srcpad
		// of the element called "stream"
		{
			stream_element = gst_bin_get_by_name(GST_BIN(cmdline_bin), "stream");
			if (stream_element == nullptr)
				throw std::runtime_error("no element with name \"stream\" found");

			GstPad *srcpad = gst_element_get_static_pad(stream_element, "src");
			if (srcpad == nullptr)
				throw std::runtime_error("no \"src\" pad in element \"stream\" found\n");
			gst_element_add_pad(GST_ELEMENT(cmdline_bin), gst_ghost_pad_new("src", srcpad));
			gst_object_unref(GST_OBJECT(srcpad));

			gst_object_unref(GST_OBJECT(stream_element));
		}


		// Setup the multisocketsink

		m_multisocketsink = gst_element_factory_make("multisocketsink", nullptr);
		if (m_multisocketsink == nullptr)
			throw std::runtime_error("could not create multisocketsink");

		g_object_set(
			m_multisocketsink,
			"unit-format", GST_FORMAT_TIME,
			"units-max", (gint64) 7 * GST_SECOND,
			"units-soft-max", (gint64) 3 * GST_SECOND,
			"recover-policy", 3 /* keyframe */ ,
			"timeout", (guint64) 10 * GST_SECOND,
			"sync-method", 1 /* next-keyframe */ ,
			nullptr
		);

		g_signal_connect(m_multisocketsink, "client-socket-removed", G_CALLBACK(on_client_socket_removed), this);


		// Setup the pipeline element & its bus watch

		m_pipeline = gst_pipeline_new(nullptr);
		g_assert(m_pipeline != nullptr);

		GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
		gst_bus_add_watch(
			bus,
			[](GstBus *p_bus, GstMessage *p_msg, gpointer p_user_data) -> gboolean
			{
				http_stream_pipeline *self = reinterpret_cast < http_stream_pipeline* > (p_user_data);
				return self->bus_watch(p_bus, p_msg);
			},
			gpointer(this)
		);
		gst_object_unref(GST_OBJECT(bus));

		// Add the other elements to the pipeline (which transfers ownership
		// over the elements to m_pipeline) and link it all together
		gst_bin_add_many(GST_BIN(m_pipeline), cmdline_bin, m_multisocketsink, nullptr);
		gst_element_link(cmdline_bin, m_multisocketsink);


		// The pipeline element now contains all the others and took
		// ownership over them, making the guard unnecessary. If something
		// goes wrong, only the pipeline element itself has to be unref'd now.
		elements_guard.dismiss();


		// Try to switch the pipeline's state to READY as the last step
		if (gst_element_set_state(m_pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
		{
			gst_object_unref(GST_OBJECT(m_pipeline));
			m_pipeline = nullptr;
			throw std::runtime_error("failed to set pipeline state to READY");
		}
	}

	~http_stream_pipeline()
	{
		if (m_pipeline != nullptr)
		{
			gst_element_set_state(m_pipeline, GST_STATE_NULL);
			gst_object_unref(GST_OBJECT(m_pipeline));
		}
	}

	void play(bool const p_do_play)
	{
		if (gst_element_set_state(m_pipeline, p_do_play ? GST_STATE_PLAYING : GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
			throw std::runtime_error("failed to set pipeline state");
	}

	std::string const & get_content_type() const
	{
		return m_content_type;
	}

	void add_client(GIOStream *p_stream, GSocket *p_socket)
	{
		// Guard against race conditions, since the m_clients
		// collection might be accessed in the streaming thread
		std::lock_guard < std::mutex > lock(m_client_mutex);

		m_clients[p_socket] = p_stream;
		g_signal_emit_by_name(m_multisocketsink, "add", p_socket);

		std::cerr << "Adding socket " << std::hex << guintptr(p_socket) << std::dec << "\n";

		// If no clients were connected until now, start/resume the pipeline
		if (m_clients.size() == 1)
		{
			std::cerr << "A client just connected, and pipeline isn't running yet - setting pipeline state to PLAYING\n";
			play(true);
		}
	}


private:
	static void on_client_socket_removed(GstElement *p_element, GSocket *p_socket, gpointer p_user_data)
	{
		http_stream_pipeline *self = reinterpret_cast < http_stream_pipeline* > (p_user_data);

		// Guard against race conditions, since this callback
		// is executed in the streaming thread
		std::lock_guard < std::mutex > lock(self->m_client_mutex);

		std::cerr << "Client with socket " << std::hex << guintptr(p_socket) << std::dec << " got removed\n";

		// Find the socket in the clients list
		auto iter = self->m_clients.find(p_socket);
		if (iter == self->m_clients.end())
		{
			std::cerr << "Socket is not in list - ignoring\n";
			return;
		}

		// Close the GIOStream, disconnecting the client
		g_io_stream_close(iter->second, nullptr, nullptr);

		// Remove the client from the collection
		self->m_clients.erase(iter);

		// Was this the last client? If so, halt the pipeline.
		// Don't call play(false) here directly, since setting the
		// state from within the streaming thread is not possible.
		// Instead, post a message that is then handled in bus_watch().
		if (self->m_clients.empty())
		{
			std::cerr << "No clients connected - setting pipeline state to READY\n";
			gst_element_post_message(
				p_element,
				gst_message_new_element(GST_OBJECT(p_element), gst_structure_new_empty("StopPipeline"))
			);
		}
	}

	bool bus_watch(GstBus *, GstMessage *p_message)
	{
		switch (GST_MESSAGE_TYPE(p_message))
		{
			case GST_MESSAGE_STATE_CHANGED:
			{
				// Only consider state change messages coming from
				// the toplevel element.
				if (GST_MESSAGE_SRC(p_message) != GST_OBJECT(m_pipeline))
					break;

				GstState old_gst_state, new_gst_state, pending_gst_state;
				gst_message_parse_state_changed(p_message, &old_gst_state, &new_gst_state, &pending_gst_state);

				auto get_dot_dump_name = [old_gst_state, new_gst_state, pending_gst_state]() -> std::string
				{
					return std::string("statechange-") +
					       "old-" + gst_element_state_get_name(old_gst_state) + "-" +
					       "cur-" + gst_element_state_get_name(new_gst_state) + "-" +
					       "pending-" + gst_element_state_get_name(pending_gst_state);
				};

				std::cerr << "State change: "
					<< " old " << gst_element_state_get_name(old_gst_state)
					<< " new " << gst_element_state_get_name(new_gst_state)
					<< " pending " << gst_element_state_get_name(pending_gst_state)
					<< "\n";

				// If the GST_DEBUG_DUMP_DOT_DIR environment variable
				// is set to a valid path, this creates a .dot dump
				// of the current pipeline structure. This is useful
				// for debugging.
				GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, get_dot_dump_name().c_str());

				break;
			}

			case GST_MESSAGE_ELEMENT:
				// This is sent by on_client_socket_removed() in case there
				// are no more clients connected.
				if (gst_message_has_name(p_message, "StopPipeline"))
					play(false);
				break;

			case GST_MESSAGE_EOS:
			{
				// Stop and tear down pipeline when EOS is reached
				std::cerr << "EOS received - halting pipeline\n";
				play(false);

				// Clear all sockets. This will invoke on_client_socket_removed()
				// for each one of them, which in turn means that all of the
				// associated GIOStreams will be closed & the m_clients collection
				// will be emptied. This way, it is ensured that all clients are
				// disconnected, which is the proper way to let them know that
				// transmission is over (since the Soup encoding in use is
				// SOUP_ENCODING_EOF).
				g_signal_emit_by_name(m_multisocketsink, "clear");

				break;
			}

			case GST_MESSAGE_INFO:
			case GST_MESSAGE_WARNING:
			case GST_MESSAGE_ERROR:
			{
				// Log the info/warning/error

				GError *gerror = nullptr;
				gchar *debug_info = nullptr;

				switch (GST_MESSAGE_TYPE(p_message))
				{
					case GST_MESSAGE_INFO:
						gst_message_parse_info(p_message, &gerror, &debug_info);
						std::cerr << "INFO: ";
						break;

					case GST_MESSAGE_WARNING:
						gst_message_parse_warning(p_message, &gerror, &debug_info);
						std::cerr << "WARNING: ";
						break;

					case GST_MESSAGE_ERROR:
						gst_message_parse_error(p_message, &gerror, &debug_info);
						std::cerr << "ERROR: ";
						break;

					default:
						g_assert_not_reached();
				}

				std::cerr << gerror->message << "; debug info: " << debug_info << "\n";

				g_clear_error(&gerror);
				g_free(debug_info);

				// In case of an error, create a dot dump and stop the pipeline
				if (GST_MESSAGE_TYPE(p_message) == GST_MESSAGE_ERROR)
				{
					GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "error");

					std::cerr << "Stopping pipeline due to error\n";

					// Stop the pipeline just like how
					// it is done with EOS messages
					play(false);
					g_signal_emit_by_name(m_multisocketsink, "clear");
				}

				break;
			}

			case GST_MESSAGE_REQUEST_STATE:
			{
				// Some element might have requested a state change.
				// Follow this request. Since the requested change
				// is done by a regular gst_element_set_state() call,
				// the pipeline will eventually produce a statechange
				// message, which is handled above. So, we do not
				// have to handle anything about the request here
				// further once gst_element_set_state() was called.

				GstState requested_state;
				gst_message_parse_request_state(p_message, &requested_state);

				std::cerr << "State change to " << gst_element_state_get_name(requested_state) << " was requested by " << GST_MESSAGE_SRC_NAME(p_message) << "\n";

				gst_element_set_state(GST_ELEMENT(m_pipeline), requested_state);

				break;
			}

			case GST_MESSAGE_LATENCY:
			{
				std::cerr << "Redistributing latency\n";
				gst_bin_recalculate_latency(GST_BIN(m_pipeline));
				break;
			}

			default:
				break;
		}

		return true;
	}


	typedef std::map < GSocket* , GIOStream* > clients;

	GstElement *m_pipeline, *m_multisocketsink;
	std::string m_content_type;
	clients m_clients;
	std::mutex m_client_mutex;
};




struct request_context
{
	SoupClientContext *m_client;
	http_stream_pipeline *m_pipeline;
};


void http_request_handler(SoupServer *, SoupMessage *p_msg, char const *, GHashTable *, SoupClientContext *p_client, gpointer p_user_data)
{
	http_stream_pipeline *pipeline = reinterpret_cast < http_stream_pipeline* > (p_user_data);

	// Set up the HTTP response headers. Use HTTP 1.0 (1.1 is not needed here).
	// We intend to transmit an open-ended stream until we close the socket
	// (because of an error or because EOS was reached), or the client disconnects.
	// This means we need EOF encoding (= data ends when the socket is closed).
	soup_message_set_http_version(p_msg, SOUP_HTTP_1_0);
	soup_message_headers_set_encoding(p_msg->response_headers, SOUP_ENCODING_EOF);
	soup_message_headers_set_content_type(p_msg->response_headers, pipeline->get_content_type().c_str(), nullptr);
	soup_message_set_status(p_msg, SOUP_STATUS_OK);

	// Context for the wrote-headers callback below
	request_context *context = new request_context { p_client, pipeline };

	// Once the HTTP response headers have all been written, steal the connection
	// and add the client. The idea is that once the headers are written, GStreamer
	// (more specifically, the multisocketsink) should take over the connection,
	// since we won't pass any data over the libsoup message body write functions
	// anyway. So, let's just take over the connection and hand it over to the
	// multisocketsink. (Keep a pointer to the GIOStream around to be able to
	// close the stream if EOS is reached or an error occurs).
	void (*wrote_headers_cb)(GObject *, GParamSpec *, gpointer) = [](GObject *, GParamSpec *, gpointer p_user_data)
	{
		request_context *context_ = reinterpret_cast < request_context* > (p_user_data);

		GSocket *socket = soup_client_context_get_gsocket(context_->m_client);
		GIOStream *stream = soup_client_context_steal_connection(context_->m_client);

		context_->m_pipeline->add_client(stream, socket);
	};
	g_signal_connect(G_OBJECT(p_msg), "wrote-headers", G_CALLBACK(wrote_headers_cb), context);
}


} // unnamed namespace end




int main(int argc, char *argv[])
{
	// First, let GStreamer parse & filter the arguments
	gst_init(&argc, &argv);

	// Check if there are enough arguments left
	if (argc < 5)
	{
		std::cerr << "Usage: " << argv[0] << " PORT CONTENT-TYPE <launch line>\n";
		std::cerr << "Example: " << argv[0] << " 8080 ( videotestsrc ! theoraenc ! oggmux name=stream )\n";
		return -1;
	}


	// Setup the libsoup server
	SoupServer *soup_server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "gst-soup-server-example", nullptr);
	if (soup_server == nullptr)
	{
		std::cerr << "Could not create Soup server\n";
		return -1;
	}

	// Add a scope guard to guarantee it is unref'd
	auto soup_server_guard = make_scope_guard([=]() { g_object_unref(G_OBJECT(soup_server)); });


	// Setup the GLib mainloop
	GMainLoop *mainloop = g_main_loop_new(nullptr, FALSE);
	if (mainloop == nullptr)
	{
		std::cerr << "Could not create GLib mainloop\n";
		return -1;
	}

	// Add a scope guard to guarantee it is unref'd
	auto mainloop_guard = make_scope_guard([=]() { g_main_loop_unref(mainloop); });


	// Install Unix signal handlers to ensure clean
	// shutdown even if for example the user presses Ctrl+C
	g_unix_signal_add(SIGINT, exit_sighandler, mainloop);
	g_unix_signal_add(SIGTERM, exit_sighandler, mainloop);


	// Get the HTTP server listening port number
	guint port;
	try
	{
		port = std::stoi(argv[1]);
	}
	catch (std::exception const &)
	{
		std::cerr << "Invalid port number \"" << argv[1] << "\"\n";
		return -1;
	}


	// Start the pipeline, install the HTTP request handler,
	// start listening, and start the mainloop
	try
	{
		http_stream_pipeline pipeline(argv[2], &argv[3]);

		soup_server_add_handler(soup_server, "/", http_request_handler, &pipeline, nullptr);

		GError *gerror = nullptr;
		if (!soup_server_listen_all(soup_server, port, SoupServerListenOptions(0), &gerror))
		{
			std::cerr << "could not start listening: " << gerror->message << "\n";
			g_clear_error(&gerror);
			return -1;
		}

		std::cerr << "Listening for incoming HTTP requests on port " << port << "\n";

		g_main_loop_run(mainloop);
	}
	catch (std::exception const &p_exc)
	{
		std::cerr << "Exception caught: " << p_exc.what() << "\n";
	}


	std::cerr << "Quitting\n";
	return 0;
}

Example code for implementing a streaming HTTP server using GStreamer and libsoup
=================================================================================

This is a simple example that shows how to combine libsoup and GStreamer to
implement an HTTP server that streams media.

To build, run `./waf configure build`. You'll need GStreamer 1.x (tested
with 1.8.2), glib 2.32.0 or newer, and libsoup 2.25.92 or newer (this is
the first release with support for EOF encoding).

Running the server requires a port number, a MIME type that is used for the
HTTP Content-Type response header, and a pipeline description. Syntax is:

    build/gst-soup-server-example PORT CONTENT-TYPE <launch line>

The launch line must have a final downstream element called "stream" with
exactly one source pad, and this source pad must be unlinked.

This example pipeline produces an h.264 stream, encapsulates it in MPEG-TS,
and listens to port 14444 for HTTP GET requests:

    build/gst-soup-server-example 14444 video/mpegts \( videotestsrc pattern=ball ! x264enc tune=0x4 key-int-max=2 ! "video/x-h264, profile=constrained-baseline" ! mpegtsmux name=stream \)

(x264enc properties and the h264 profile are chosen to produce a stream with
minimal latency and very frequent keyframes to allow the clients to start
playback quickly.)

If for example gst-soup-server-example is running on a machine with IP address
192.168.1.190, then a client can start playing by issuing a GET request to the
URL `http://192.168.1.190:14444/` .

The server sets the pipeline to PLAYING once a client connects. If a second
client connects, the stream is shared. When all clients disconnect, the
pipeline is set back to the READY state.

In case the pipeline encounters the EOS event, the pipeline is put to the
READY state, and all connections are closed.

NOTE: This example expects the user to specify a content MIME type. It is
theoretically possible to extend the code to not need that, and instead figure
out a MIME type based on the source GstCaps the "stream" element produces.
However, this adds some complexity to the code, so in order to keep it simple,
this was omitted.

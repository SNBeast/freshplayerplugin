#!/usr/bin/env bash

set -euo pipefail

SDK_DIR="$1"
FFMPEG_VER="$2"
AVCODEC_SO="$3"
AVUTIL_SO="$4"

INC_DIR="$SDK_DIR/include"
LIB_DIR="$SDK_DIR/lib"
SRC_DIR="$SDK_DIR/src"

mkdir -p "$INC_DIR/libavcodec" "$INC_DIR/libavutil" "$LIB_DIR" "$SRC_DIR"

TARBALL="ffmpeg-$FFMPEG_VER.tar.xz"
if [ ! -f "/tmp/$TARBALL" ]; then
    curl -fsSL "https://ffmpeg.org/releases/$TARBALL" -o "/tmp/$TARBALL"
fi
tar -xJf "/tmp/$TARBALL" -C "$SRC_DIR" --strip-components=1

cp "$SRC_DIR"/libavcodec/*.h "$INC_DIR/libavcodec/"
cp "$SRC_DIR"/libavutil/*.h  "$INC_DIR/libavutil/"

cat << 'EOF' > "$INC_DIR/libavutil/avconfig.h"
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
EOF

# we need fake libraries to do a scheme of resolving the system's ffmpeg symbols

cat << 'EOF' > "$SDK_DIR/avcodec_stub.c"
void avcodec_alloc_context3(void) {}
void avcodec_find_decoder(void) {}
void avcodec_flush_buffers(void) {}
void avcodec_free_context(void) {}
void avcodec_open2(void) {}
void avcodec_receive_frame(void) {}
void avcodec_send_packet(void) {}
EOF

cat << 'EOF' > "$SDK_DIR/avutil_stub.c"
void av_packet_alloc(void) {}
void av_packet_free(void) {}
void av_parser_close(void) {}
void av_parser_init(void) {}
void av_parser_parse2(void) {}
void av_buffer_create(void) {}
void av_buffer_ref(void) {}
void av_buffer_unref(void) {}
void av_frame_alloc(void) {}
void av_frame_free(void) {}
void av_hwdevice_ctx_alloc(void) {}
void av_hwdevice_ctx_init(void) {}
void av_hwframe_ctx_alloc(void) {}
void av_hwframe_ctx_init(void) {}
EOF

# and as well we need to ensure that the shim's constructor can see the two functions it calls

cat << 'EOF' > "$SDK_DIR/pp_interface_resource_stub.c"
char config[65536];
char display[65536];
void pp_resource_acquire(void) {}
void pp_resource_allocate(void) {}
void pp_resource_get_type(void) {}
void pp_resource_ref(void) {}
void pp_resource_release(void) {}
void pp_resource_unref(void) {}
void ppb_buffer_map(void) {}
void ppb_buffer_unmap(void) {}
void ppb_graphics3d_create(void) {}
void ppb_message_loop_get_current(void) {}
void ppb_message_loop_post_work_with_result(void) {}
void ppp_get_interface(void) {}
void register_interface(void) {}
void register_resource(void) {}
void tables_get_pp_instance(void) {}
void trace_error(void) {}
void trace_warning(void) {}
EOF

gcc -shared -fPIC -Wl,-soname,"$AVCODEC_SO" "$SDK_DIR/avcodec_stub.c" -o "$LIB_DIR/$AVCODEC_SO"
gcc -shared -fPIC -Wl,-soname,"$AVUTIL_SO"  "$SDK_DIR/avutil_stub.c"  -o "$LIB_DIR/$AVUTIL_SO"

gcc -shared -fPIC "$SDK_DIR/pp_interface_resource_stub.c" -o "$LIB_DIR/libpp_interface_resource_stub.so"

ln -sf "$AVCODEC_SO" "$LIB_DIR/libavcodec.so"
ln -sf "$AVUTIL_SO"  "$LIB_DIR/libavutil.so"

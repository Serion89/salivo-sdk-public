/* Salivo Stage 36.1 async runtime - platforms without a backend.
 *
 * Platforms without a Stage 36.1 backend (Windows IOCP, POSIX epoll/poll). Every entry point here fails with
 * Unsupported (-12) so a program on another platform fails clearly instead of silently
 * running a fake runtime. Generated from salivo_aio_abi.def (scripts/stage36_1.py --gen-stubs).
 */
#include <stdio.h>

static int64_t sa_unsupported(void) {
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[salivo-aio] async runtime backend not available on this platform (Stage 36.1 backends: Windows, Linux, macOS)\n");
    }
    return -12;
}

int64_t salivo_aio_abi_version(void) {  return sa_unsupported(); }
int64_t salivo_aio_create(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_use(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_start(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_run(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_shutdown(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_join(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_destroy(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_stat(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_last_value(void) {  return sa_unsupported(); }
char* salivo_aio_last_text_str(void) {  sa_unsupported(); return (char*)"Unsupported"; }
char* salivo_aio_last_peer_str(void) {  sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_last_os_error(void) {  return sa_unsupported(); }
int64_t salivo_aio_worker_id(void) {  return sa_unsupported(); }
int64_t salivo_aio_spawn(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_spawn_owning(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_spawn_on(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_await(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_await_timeout(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_release(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_cancel(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_is_cancelled(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_task_state(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_yield(void) {  return sa_unsupported(); }
int64_t salivo_aio_safepoint(void) {  return sa_unsupported(); }
int64_t salivo_aio_current(void) {  return sa_unsupported(); }
int64_t salivo_aio_fail(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_sleep(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_blocking(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_chan_new(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_send(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_chan_try_send(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_chan_recv(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_try_recv(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_close(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_len(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_new_text(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_send_text(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_chan_try_send_text(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_chan_recv_text(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_chan_try_recv_text(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_tcp_listen(const char* a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_tcp_accept(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_tcp_connect(const char* a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_read(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_write(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_write_text(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_tcp_shutdown(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_close(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_local_port(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_udp_bind(const char* a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_udp_send_to(int64_t a0, int64_t a1, int64_t a2, const char* a3, int64_t a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return sa_unsupported(); }
int64_t salivo_aio_udp_recv_from(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_file_open(const char* a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_file_seek(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_flush(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_buf_new(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_buf_free(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_buf_get(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_buf_set(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_buf_put_text(int64_t a0, int64_t a1, const char* a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
char* salivo_aio_buf_str(int64_t a0, int64_t a1) { (void)a0; (void)a1; sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_arg_new(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_arg_set_int(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_arg_set_text(int64_t a0, int64_t a1, const char* a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_arg_int(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
char* salivo_aio_arg_get_str(int64_t a0, int64_t a1) { (void)a0; (void)a1; sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_arg_free(int64_t a0) { (void)a0; return sa_unsupported(); }
char* salivo_aio_error_name_str(int64_t a0) { (void)a0; sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_http_server(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_client(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_set(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_next(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_request(int64_t a0, const char* a1, const char* a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_add_header(int64_t a0, const char* a1, const char* a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_send(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_write(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_write_text(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_finish(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_response(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_read(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; return sa_unsupported(); }
int64_t salivo_aio_http_next_chunk(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_chunk_byte(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
char* salivo_aio_http_text_str(int64_t a0, int64_t a1) { (void)a0; (void)a1; sa_unsupported(); return (char*)"Unsupported"; }
char* salivo_aio_http_header_str(int64_t a0, const char* a1, int64_t a2) { (void)a0; (void)a1; (void)a2; sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_http_header_count(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
char* salivo_aio_http_header_at_str(int64_t a0, int64_t a1, int64_t a2) { (void)a0; (void)a1; (void)a2; sa_unsupported(); return (char*)"Unsupported"; }
char* salivo_aio_http_trailer_str(int64_t a0, const char* a1) { (void)a0; (void)a1; sa_unsupported(); return (char*)"Unsupported"; }
int64_t salivo_aio_http_info(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_reset(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_free(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_goaway(int64_t a0, int64_t a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_ping(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_close(int64_t a0) { (void)a0; return sa_unsupported(); }
int64_t salivo_aio_http_authority(int64_t a0, const char* a1) { (void)a0; (void)a1; return sa_unsupported(); }
int64_t salivo_aio_http_stat(int64_t a0) { (void)a0; return sa_unsupported(); }

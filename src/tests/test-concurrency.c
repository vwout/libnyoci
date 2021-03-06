/*!	@page test-concurrency test-concurrency.c: Concurrency test.
**
**	This test exercises using two separate LibNyoci instances on different
**  threads.
**
**	@include test-concurrency.c
**
*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include "assert-macros.h"
#include <stdio.h>
#include <pthread.h>
#include <libnyoci/libnyoci.h>

#define NUMBER_OF_THREADS			(10)
#define TRANSACTIONS_PER_THREAD		(10)

#if !VERBOSE_DEBUG
#define printf(...)		do { } while(0)
#endif

char* gMainThreadURL;

struct test_concurrency_thread_s {
	pthread_t pt;
	nyoci_transaction_t transaction;
	int remaining;
	int index;
};

static nyoci_status_t
request_handler(void* context) {
	/* This request handler will respond to every GET request with "Hello world!".
	 * It isn't usually a good idea to (almost) completely ignore the actual
	 * request, but since this is just an example we can get away with it.
	 * We do more processing on the request in the other examples. */

	printf("MAIN: Got a request!\n");

	// Only handle GET requests for now. Returning NYOCI_STATUS_NOT_IMPLEMENTED
	// here without sending a response will cause us to automatically
	// send a METHOD_NOT_IMPLEMENTED response.
	if(nyoci_inbound_get_code() != COAP_METHOD_GET)
		return NYOCI_STATUS_NOT_IMPLEMENTED;

	// Begin describing the response message. (2.05 CONTENT,
	// in this case)
	nyoci_outbound_begin_response(COAP_RESULT_205_CONTENT);

	// Add an option describing the content type as plaintext.
	nyoci_outbound_add_option_uint(
		COAP_OPTION_CONTENT_TYPE,
		COAP_CONTENT_TYPE_TEXT_PLAIN
	);

	// Set the content of our response to be "Hello world!".
	nyoci_outbound_append_content("Hello world!", NYOCI_CSTR_LEN);

	// Send the response we hae created, passing the return value
	// to our caller.
	return nyoci_outbound_send();
}

nyoci_status_t
test_concurrency_thread_resend(void* context)
{
//	struct test_concurrency_thread_s* obj = (struct test_concurrency_thread_s*)context;
	nyoci_status_t status = 0;

	status = nyoci_outbound_begin(
		nyoci_get_current_instance(),
		COAP_METHOD_GET,
		COAP_TRANS_TYPE_CONFIRMABLE
	);
	require_noerr(status,bail);

	status = nyoci_outbound_set_uri(gMainThreadURL, 0);
	require_noerr(status,bail);

	status = nyoci_outbound_send();

	if(status) {
		check_noerr(status);
		fprintf(
			stderr,
			"nyoci_outbound_send() returned error %d(%s).\n",
			status,
			nyoci_status_to_cstr(status)
		);
		goto bail;
	}

bail:
	if(status == 0) {
		printf("%d: Sent Request.\n",obj->index);
	} else {
		printf("%d: Request Delayed: %d (%s)\n",obj->index, status, nyoci_status_to_cstr(status));
		if(status == NYOCI_STATUS_HOST_LOOKUP_FAILURE) {
			status = NYOCI_STATUS_WAIT_FOR_DNS;
		}
	}

	return status;
}

nyoci_status_t
test_concurrency_thread_response(int statuscode, void* context)
{
	struct test_concurrency_thread_s* obj = (struct test_concurrency_thread_s*)context;

	if (statuscode == NYOCI_STATUS_TRANSACTION_INVALIDATED) {
		printf("%d: Transaction invalidated\n", obj->index);
		obj->remaining--;
		obj->transaction = NULL;
	} else if (statuscode == COAP_RESULT_205_CONTENT) {
		printf("%d: Got content: %s\n", obj->index, nyoci_inbound_get_content_ptr());
	} else {
		printf("%d: ERROR: Got unexpected status code %d (%s)\n", obj->index,statuscode,nyoci_status_to_cstr(statuscode));
		exit(EXIT_FAILURE);
	}
	return 0;
}

void*
thread_main(void* context)
{
	struct test_concurrency_thread_s* obj = (struct test_concurrency_thread_s*)context;
	nyoci_t instance;

	obj->remaining = TRANSACTIONS_PER_THREAD;
	obj->transaction = NULL;

	// Create our instance on the default CoAP port. If the port
	// is already in use, we will pick the next available port number.
	instance = nyoci_create();

	if(!instance) {
		perror("Unable to create second LibNyoci instance");
		exit(EXIT_FAILURE);
	}

	nyoci_plat_bind_to_port(instance, NYOCI_SESSION_TYPE_UDP, 0);

	printf("%d: Thread Listening on port %d\n",obj->index,nyoci_plat_get_port(instance));

	while (obj->remaining > 0) {
		if (obj->transaction == NULL) {
			printf("%d: Starting transaction #%d\n",obj->index, TRANSACTIONS_PER_THREAD-obj->remaining+1);
			obj->transaction = nyoci_transaction_init(
				NULL,
				NYOCI_TRANSACTION_ALWAYS_INVALIDATE,
				&test_concurrency_thread_resend,
				&test_concurrency_thread_response,
				(void*)obj
			);
			nyoci_transaction_begin(
				instance,
				obj->transaction,
				3*MSEC_PER_SEC
			);
		}
		nyoci_plat_wait(instance, 5000);
		nyoci_plat_process(instance);
	}

	obj->remaining = 0;

	return NULL;
}

int
main(void)
{
#if NYOCI_SINGLETON
	// We skip this test when built as a singleton
	// because there is only ever one instance.
	printf("SKIP\n");
	return EXIT_SUCCESS;
#endif

	nyoci_t instance;
	struct test_concurrency_thread_s threads[NUMBER_OF_THREADS] = { };
	int i;
	bool is_finished = false;
	nyoci_timestamp_t start_time = nyoci_plat_cms_to_timestamp(0);

	NYOCI_LIBRARY_VERSION_CHECK();

	instance = nyoci_create();

	if(!instance) {
		perror("Unable to create first LibNyoci instance");
		exit(EXIT_FAILURE);
	}

	nyoci_plat_bind_to_port(instance, NYOCI_SESSION_TYPE_UDP, 0);

	printf("Main Listening on port %d\n", nyoci_plat_get_port(instance));

	asprintf(&gMainThreadURL, "coap://localhost:%d/", nyoci_plat_get_port(instance));

	nyoci_set_default_request_handler(instance, &request_handler, NULL);

	for (i = 0; i < sizeof(threads)/sizeof(*threads); i++) {
		threads[i].index = i;
		pthread_create(
			&threads[i].pt,
			NULL,
			(void*(*)(void*))&thread_main,
			(void*)&threads[i]
		);
	}

	while (!is_finished) {
		if (-nyoci_plat_timestamp_to_cms(start_time) > MSEC_PER_SEC*10) {
			fprintf(stderr,"TIMEOUT\n");
			return EXIT_FAILURE;
		}
		nyoci_plat_process(instance);
		if (nyoci_plat_wait(instance, 1000) == NYOCI_STATUS_TIMEOUT) {
			is_finished = true;
			for (i = 0; i < sizeof(threads)/sizeof(*threads); i++) {
				if (threads[i].remaining > 0) {
					is_finished = false;
					break;
				}
			}
		}
	}

	nyoci_release(instance);

	return EXIT_SUCCESS;
}

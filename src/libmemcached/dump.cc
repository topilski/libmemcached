/*
    +--------------------------------------------------------------------+
    | libmemcached-awesome - C/C++ Client Library for memcached          |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted under the terms of the BSD license.    |
    | You should have received a copy of the license in a bundled file   |
    | named LICENSE; in case you did not receive a copy you can review   |
    | the terms online at: https://opensource.org/licenses/BSD-3-Clause  |
    +--------------------------------------------------------------------+
    | Copyright (c) 2006-2014 Brian Aker   https://datadifferential.com/ |
    | Copyright (c) 2020-2021 Michael Wallner        https://awesome.co/ |
    +--------------------------------------------------------------------+
*/

#include "libmemcached/common.h"

static memcached_return_t ascii_dump(Memcached *memc, memcached_dump_fn *callback, void *context,
                                     uint32_t number_of_callbacks) {
  memcached_version(memc);
  /* MAX_NUMBER_OF_SLAB_CLASSES is defined to 200 in Memcached 1.4.10 */
  for (uint32_t x = 0; x < 200; x++) {
    char buffer[MEMCACHED_DEFAULT_COMMAND_SIZE];
    int buffer_length = snprintf(buffer, sizeof(buffer), "%u", x);
    if (size_t(buffer_length) >= sizeof(buffer) or buffer_length < 0) {
      return memcached_set_error(
          *memc, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT,
          memcached_literal_param("snprintf(MEMCACHED_DEFAULT_COMMAND_SIZE)"));
    }

    // @NOTE the hard coded zero means "no limit"
    libmemcached_io_vector_st vector[] = {{memcached_literal_param("stats cachedump ")},
                                          {buffer, size_t(buffer_length)},
                                          {memcached_literal_param(" 0\r\n")}};

    // Send message to all servers
    for (uint32_t server_key = 0; server_key < memcached_server_count(memc); server_key++) {
      memcached_instance_st *instance = memcached_instance_fetch(memc, server_key);

      // skip slabs >63 for server versions >= 1.4.23
      if (x < 64 || memcached_version_instance_cmp(instance, 1, 4, 23) < 0) {
        memcached_return_t vdo_rc;
        if (memcached_failed((vdo_rc = memcached_vdo(instance, vector, 3, true)))) {
          return vdo_rc;
        }
      }
    }

    // Collect the returned items
    memcached_instance_st *instance;
    memcached_return_t read_ret = MEMCACHED_SUCCESS;
    while ((instance = memcached_io_get_readable_server(memc, read_ret))) {
      memcached_return_t response_rc =
          memcached_response(instance, buffer, MEMCACHED_DEFAULT_COMMAND_SIZE, NULL);
      if (response_rc == MEMCACHED_ITEM) {
        char *string_ptr, *end_ptr;

        string_ptr = buffer;
        string_ptr += 5; /* Move past ITEM */

        for (end_ptr = string_ptr; isgraph(*end_ptr); end_ptr++) {
        };

        char *key = string_ptr;
        key[(size_t)(end_ptr - string_ptr)] = 0;

        int bytes = 0;
        unsigned long long expire_time = 0;
        sscanf(end_ptr + 1, "[%d b; %llu s;]", &bytes, &expire_time);
        for (uint32_t callback_counter = 0; callback_counter < number_of_callbacks;
             callback_counter++) {
          memcached_return_t callback_rc =
              (*callback[callback_counter])(memc, key, (size_t)(end_ptr - string_ptr), expire_time, context);
          if (callback_rc != MEMCACHED_SUCCESS) {
            // @todo build up a message for the error from the value
            memcached_set_error(*instance, callback_rc, MEMCACHED_AT);
            break;
          }
        }
      } else if (response_rc == MEMCACHED_END) {
        // All items have been returned
      } else if (response_rc == MEMCACHED_SERVER_ERROR) {
        /* If we try to request stats cachedump for a slab class that is too big
         * the server will return an incorrect error message:
         * "MEMCACHED_SERVER_ERROR failed to allocate memory"
         * This isn't really a fatal error, so let's just skip it. I want to
         * fix the return value from the memcached server to a CLIENT_ERROR,
         * so let's add support for that as well right now.
         */
        assert(response_rc == MEMCACHED_SUCCESS); // Just fail
        return response_rc;
      } else if (response_rc == MEMCACHED_CLIENT_ERROR) {
        /* The maximum number of slabs has changed in the past (currently 1<<6-1),
         * so ignore any client errors complaining about an illegal slab id.
         */
        if (0
            == strncmp(buffer, "CLIENT_ERROR Illegal slab id",
                       sizeof("CLIENT_ERROR Illegal slab id") - 1))
        {
          memcached_error_free(*instance);
          memcached_error_free(*memc);
        } else {
          return response_rc;
        }
      } else {
        // IO error of some sort must have occurred
        return response_rc;
      }
    }
  }

  return memcached_has_current_error(*memc) ? MEMCACHED_SOME_ERRORS : MEMCACHED_SUCCESS;
}

memcached_return_t memcached_dump(memcached_st *shell, memcached_dump_fn *callback, void *context,
                                  uint32_t number_of_callbacks) {
  Memcached *ptr = memcached2Memcached(shell);
  memcached_return_t rc;
  if (memcached_failed(rc = initialize_query(ptr, true))) {
    return rc;
  }

  /*
    No support for Binary protocol yet
    @todo Fix this so that we just flush, switch to ascii, and then go back to binary.
  */
  if (memcached_is_binary(ptr)) {
    return memcached_set_error(
        *ptr, MEMCACHED_NOT_SUPPORTED, MEMCACHED_AT,
        memcached_literal_param("Binary protocol is not supported for memcached_dump()"));
  }

  return ascii_dump(ptr, callback, context, number_of_callbacks);
}

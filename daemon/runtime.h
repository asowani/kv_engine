/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
*     Copyright 2015 Couchbase, Inc
*
*   Licensed under the Apache License, Version 2.0 (the "License");
*   you may not use this file except in compliance with the License.
*   You may obtain a copy of the License at
*
*       http://www.apache.org/licenses/LICENSE-2.0
*
*   Unless required by applicable law or agreed to in writing, software
*   distributed under the License is distributed on an "AS IS" BASIS,
*   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*   See the License for the specific language governing permissions and
*   limitations under the License.
*/

/*
 * Due to the fact that the memcached daemon is written in C we need
 * this little wrapper to provide atomics functionality without having
 * to reinvent the wheel
 */
#pragma once

#include <memcached/openssl.h>

#ifdef __cplusplus
extern "C" {
#endif

    void set_ssl_cipher_list(const char *new_list);
    void set_ssl_ctx_cipher_list(SSL_CTX *ctx);

    /**
     * Set the SSL protocol mask used to filter out
     * which SSL protocols we accept (TLSv1, TLSv1.1, TLSv1.2 etc)
     *
     * (The primary reason for this code to exist is to be a bridge
     * between C and C++ (the mask is stored in a std::atomic to allow
     * it to be accessed from multiple threads)
     *
     * @param mask (or could also be called minimum) is the minimum
     *             SSL protocol we're supporting. Specifying TLSv1.2
     *             would set the new mask to mask out SSLv2, SSLv3,
     *             TLSv1 and TLSv1.1
     */
    void set_ssl_protocol_mask(const char* mask);

    /**
     * Set the current ssl protocol mask to the newly created SSL
     * context. (The primary reason for this code to exist is to
     * be a bridge between C and C++ (the mask is stored in a
     * std::atomic to allow it to be accessed from multiple threads)
     *
     * @param ctx the SSL context to update with the SSL protocol
     *            mask.
     */
    void set_ssl_ctx_protocol_mask(SSL_CTX* ctx);

#ifdef __cplusplus
}
#endif

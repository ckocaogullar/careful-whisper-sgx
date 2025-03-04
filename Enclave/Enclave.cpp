/*

Copyright 2018 Intel Corporation

This software and the related documents are Intel copyrighted materials,
and your use of them is governed by the express license under which they
were provided to you (License). Unless the License provides otherwise,
you may not use, modify, copy, publish, distribute, disclose or transmit
this software or the related documents without Intel's prior written
permission.

This software and the related documents are provided as is, with no
express or implied warranties, other than those that are expressly stated
in the License.

*/

#ifndef _WIN32
#include "../config.h"
#endif
#include "Enclave_t.h"
#include <string.h>
#include <sgx_utils.h>
#ifdef _WIN32
#include <sgx_tae_service.h>
#endif
#include <sgx_tkey_exchange.h>
#include <sgx_tcrypto.h>
#include <stdio.h>
#include <stdarg.h>

#include <sgx_key_exchange.h>
// Manually added the include path for crypto.h and its dependencies in Makefile. Modify AM_CPPFLAGS if you would like to direct it to another path.
#include "crypto.h"

static const sgx_ec256_public_t def_service_public_key = {
    {
        0x72, 0x12, 0x8a, 0x7a, 0x17, 0x52, 0x6e, 0xbf,
        0x85, 0xd0, 0x3a, 0x62, 0x37, 0x30, 0xae, 0xad,
        0x3e, 0x3d, 0xaa, 0xee, 0x9c, 0x60, 0x73, 0x1d,
        0xb0, 0x5b, 0xe8, 0x62, 0x1c, 0x4b, 0xeb, 0x38
    },
    {
        0xd4, 0x81, 0x40, 0xd9, 0x50, 0xe2, 0x57, 0x7b,
        0x26, 0xee, 0xb7, 0x41, 0xe7, 0xc6, 0x14, 0xe2,
        0x24, 0xb7, 0xbd, 0xc9, 0x03, 0xf2, 0x9a, 0x28,
        0xa8, 0x3c, 0xc8, 0x10, 0x11, 0x14, 0x5e, 0x06
    }

};


	
#define PSE_RETRIES	5	/* Arbitrary. Not too long, not too short. */

/*----------------------------------------------------------------------
 * WARNING
 *----------------------------------------------------------------------
 *
 * End developers should not normally be calling these functions
 * directly when doing remote attestation:
 *
 *    sgx_get_ps_sec_prop()
 *    sgx_get_quote()
 *    sgx_get_quote_size()
 *    sgx_get_report()
 *    sgx_init_quote()
 *
 * These functions short-circuits the RA process in order
 * to generate an enclave quote directly!
 *
 * The high-level functions provided for remote attestation take
 * care of the low-level details of quote generation for you:
 *
 *   sgx_ra_init()
 *   sgx_ra_get_msg1
 *   sgx_ra_proc_msg2
 *
 *----------------------------------------------------------------------
 */

/*
 * This doesn't really need to be a C++ source file, but a bug in 
 * 2.1.3 and earlier implementations of the SGX SDK left a stray
 * C++ symbol in libsgx_tkey_exchange.so so it won't link without
 * a C++ compiler. Just making the source C++ was the easiest way
 * to deal with that.
 */

sgx_status_t get_report(sgx_report_t *report, sgx_target_info_t *target_info)
{
#ifdef SGX_HW_SIM
	return sgx_create_report(NULL, NULL, report);
#else
	return sgx_create_report(target_info, NULL, report);
#endif
}

#ifdef _WIN32
size_t get_pse_manifest_size ()
{
	return sizeof(sgx_ps_sec_prop_desc_t);
}

sgx_status_t get_pse_manifest(char *buf, size_t sz)
{
	sgx_ps_sec_prop_desc_t ps_sec_prop_desc;
	sgx_status_t status= SGX_ERROR_SERVICE_UNAVAILABLE;
	int retries= PSE_RETRIES;

	do {
		status= sgx_create_pse_session();
		if ( status != SGX_SUCCESS ) return status;
	} while (status == SGX_ERROR_BUSY && retries--);
	if ( status != SGX_SUCCESS ) return status;

	status= sgx_get_ps_sec_prop(&ps_sec_prop_desc);
	if ( status != SGX_SUCCESS ) return status;

	memcpy(buf, &ps_sec_prop_desc, sizeof(ps_sec_prop_desc));

	sgx_close_pse_session();

	return status;
}
#endif

sgx_status_t enclave_ra_init(sgx_ec256_public_t key, int b_pse,
	sgx_ra_context_t *ctx, sgx_status_t *pse_status)
{
	sgx_status_t ra_status;

	/*
	 * If we want platform services, we must create a PSE session 
	 * before calling sgx_ra_init()
	 */

#ifdef _WIN32
	if ( b_pse ) {
		int retries= PSE_RETRIES;
		do {
			*pse_status= sgx_create_pse_session();
			if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
		} while (*pse_status == SGX_ERROR_BUSY && retries--);
		if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
	}

	ra_status= sgx_ra_init(&key, b_pse, ctx);

	if ( b_pse ) {
		int retries= PSE_RETRIES;
		do {
			*pse_status= sgx_close_pse_session();
			if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
		} while (*pse_status == SGX_ERROR_BUSY && retries--);
		if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
	}
#else
	ra_status= sgx_ra_init(&key, 0, ctx);
#endif

	return ra_status;
}

sgx_status_t enclave_ra_init_def(int b_pse, sgx_ra_context_t *ctx,
	sgx_status_t *pse_status)
{
	return enclave_ra_init(def_service_public_key, b_pse, ctx, pse_status);
}

/*
 * Return a SHA256 hash of the requested key. KEYS SHOULD NEVER BE
 * SENT OUTSIDE THE ENCLAVE IN PLAIN TEXT. This function let's us
 * get proof of possession of the key without exposing it to untrusted
 * memory.
 */

sgx_status_t enclave_ra_get_key_hash(sgx_status_t *get_keys_ret,
	sgx_ra_context_t ctx, sgx_ra_key_type_t type, sgx_sha256_hash_t *hash)
{
	sgx_status_t sha_ret;
	sgx_ra_key_128_t k;

	// First get the requested key which is one of:
	//  * SGX_RA_KEY_MK 
	//  * SGX_RA_KEY_SK
	// per sgx_ra_get_keys().

	*get_keys_ret= sgx_ra_get_keys(ctx, type, &k);
	if ( *get_keys_ret != SGX_SUCCESS ) return *get_keys_ret;

	/* Now generate a SHA hash */

	sha_ret= sgx_sha256_msg((const uint8_t *) &k, sizeof(k), 
		(sgx_sha256_hash_t *) hash); // Sigh.

	/* Let's be thorough */

	memset(k, 0, sizeof(k));

	return sha_ret;
}

sgx_status_t enclave_ra_close(sgx_ra_context_t ctx)
{
        sgx_status_t ret;
        ret = sgx_ra_close(ctx);
        return ret;
}

// For printing stuff in the (untrusted) client
void printf(const char *fmt, ...)
{
    char buf[BUFSIZ] = {'\0'};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, BUFSIZ, fmt, ap);
    va_end(ap);
    ocall_print_string(buf);
}

// Necessary for the hexstring function -- Taken from the hexutil library
static char *_hex_buffer= NULL;
static size_t _hex_buffer_size= 0;
const char _hextable[]= "0123456789abcdef";

const char *hexstring (const void *vsrc, size_t len)
{
	size_t i, bsz;
	const unsigned char *src= (const unsigned char *) vsrc;
	char *bp;

	bsz= len*2+1;	/* Make room for NULL byte */
	if ( bsz >= _hex_buffer_size ) {
		/* Allocate in 1K increments. Make room for the NULL byte. */
		size_t newsz= 1024*(bsz/1024) + ((bsz%1024) ? 1024 : 0);
		_hex_buffer_size= newsz;
		_hex_buffer= (char *) realloc(_hex_buffer, newsz);
		if ( _hex_buffer == NULL ) {
			return "(out of memory)";
		}
	}

	for(i= 0, bp= _hex_buffer; i< len; ++i) {
		*bp= _hextable[src[i]>>4];
		++bp;
		*bp= _hextable[src[i]&0xf];
		++bp;
	}
	_hex_buffer[len*2]= 0;
	
	return (const char *) _hex_buffer;
}

int dummy_prove(){
	printf("Generating dummy proof for peer attestation.\n");
	return 1997;

}

int dummy_verify(int proof){
	printf("Here is the proof: %d\n", proof);
	if (proof == 1997){
		printf("Dummy proof from the peer is VERIFIED.\n");
	} else {
		printf("COULD NOT verify the proof from the peer.\n");
	}
}


int process_msg01 (uint32_t msg0_extended_epid_group_id, sgx_ra_msg1_t *msg1)
{
	printf("\nMsg0 Details (from Prover)\n");
	printf("msg0.extended_epid_group_id = %u\n",
			msg0_extended_epid_group_id);
	printf("\n");
	

	/* According to the Intel SGX Developer Reference
	 * "Currently, the only valid extended Intel(R) EPID group ID is zero. The
	 * server should verify this value is zero. If the Intel(R) EPID group ID 
	 * is not zero, the server aborts remote attestation"
	 */

	if ( msg0_extended_epid_group_id != 0 ) {
		printf("msg0 Extended Epid Group ID is not zero.  Exiting.\n");
		return 0;
	}

	// Pass msg1 back to the pointer in the caller func
	// memcpy(msg1, &msg01->msg1, sizeof(sgx_ra_msg1_t));
	
	printf("\nMsg1 Details (from Prover)\n");
	printf("msg1.g_a.gx = %s\n",
		hexstring(&msg1->g_a.gx, sizeof(msg1->g_a.gx)));
	printf("msg1.g_a.gy = %s\n",
		hexstring(&msg1->g_a.gy, sizeof(msg1->g_a.gy)));
	printf("msg1.gid    = %s\n",
		hexstring( &msg1->gid, sizeof(msg1->gid)));
	printf("\n");

	// /* Generate our session key */

	// printf("+++ generating session key Gb\n");

	// Gb= key_generate();
	// if ( Gb == NULL ) {
	// 	eprintf("Could not create a session key\n");
	// 	free(msg01);
	// 	return 0;
	// }

	// /*
	//  * Derive the KDK from the key (Ga) in msg1 and our session key.
	//  * An application would normally protect the KDK in memory to 
	//  * prevent trivial inspection.
	//  */

	// printf("+++ deriving KDK\n");

	// if ( ! derive_kdk(Gb, session->kdk, msg1->g_a, config) ) {
	// 	printf("Could not derive the KDK\n");
	// 	free(msg01);
	// 	return 0;
	// }

	// printf("+++ KDK = %s\n", hexstring(session->kdk, 16));

	// /*
 	//  * Derive the SMK from the KDK 
	//  * SMK = AES_CMAC(KDK, 0x01 || "SMK" || 0x00 || 0x80 || 0x00) 
	//  */

	// printf("+++ deriving SMK\n");

	// cmac128(session->kdk, (unsigned char *)("\x01SMK\x00\x80\x00"), 7,
	// 	session->smk);

	// printf("+++ SMK = %s\n", hexstring(session->smk, 16));

	return 1;
	}
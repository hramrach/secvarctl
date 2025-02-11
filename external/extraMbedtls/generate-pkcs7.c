// SPDX-License-Identifier: Apache-2.0
/* Copyright 2021 IBM Corp.*/
#include <string.h>
#include <stdlib.h>
#include <mbedtls/base64.h> // for pem 2 der functions
#include "external/extraMbedtls/include/generate-pkcs7.h"
#include "err.h"
#include "prlog.h"
#ifndef NO_CRYPTO

#include <stdio.h>

#include <mbedtls/asn1write.h> // for building pkcs7
#include <mbedtls/md.h>     //  generic interface 
#include <mbedtls/md_internal.h>
#include <mbedtls/platform.h> //  mbedtls functions
#include <mbedtls/pk_internal.h>
#include <mbedtls/x509_crt.h>
#include "generic.h"

#include "external/skiboot/libstb/secvar/backend/edk2-compat-process.h" //  work on factoring this out

extern int verbose;
/* STRUCTURE OF PKCS7 AND CORRESPONDING FUNCTIONS THAT HANDLE THEM:
 *PKCS7 {
 *	CONSTRUCTED | SEQUENCE 										->setPKCS7OID
 *		OID (Signed Data) 										->^
 *		CONSTRUCTED | CONTEXT SPECIFIC 							->setSignedData
 *			CONSTRUCTED | SEQUENCE 								->setVersion
 *				INTEGER (version) 								->^
 *				CONSTRUCTED |SET 								->setAlgoID
 *					OID (hash Alg)								->^
 *				CONSTRUCTED | SEQUENCE 							->setSignedDataOID
 *					OID (PKCS7 Data)							->^
 *				CONSTRUCTED | CONTEXT SPECIFIC 					->setSignerCertRaw
 *					entire certificate for each signer  		->^
 *				CONSTRUCTED | SET 								->setSignersData
 *					CONSTRUCTED | SEQUENCE (for each signer) 	->setSignerDataForEachSigner
 *						INTEGER (signedInfoVersion) 			->setSignerCertData
 *						CONSTRUCTED | SEQUENCE 					->^
 *							certificate issuer info 			->^
 *							certificate serial					->^
 *						CONSTRUCTED | SEQUENCE 					->setAlgorithmIDs
 *							OID (hash Alg)						->^
 *						CONSTRUCTED | SEQUENCE 					->^
 *							OID (Signature Alg (RSA)) 			->^
 *						OCTET STRING (signature) 				->setSignature
 * }
 */

typedef struct PKCS7Info {
	unsigned char **crts; // signing crt DER
	size_t *crtSizes;
	unsigned char **keys; // signing key DER or signatures (depends on alreadySignedFlag)
	size_t *keySizes;
	int keyPairs;
	const unsigned char *newData; 
	int newDataSize;
	mbedtls_md_type_t hashFunct;
	const char * hashFunctOID; 
	int alreadySignedFlag; // if this is 1 then then PKCS7Info.keys contains signatures, if 0 then contains siging key in DER format 

} PKCS7Info;
#endif

/*
 *Taken from MBEDTLS, mbedtls-mbedtls2.23.0/programs/util/pem2der.c many thanks to these great folks
 * Some things were changed though, like memory allocation
 *THIS ALLOCATES MEMORY, FREE SOMETIME AFTER CALLING
 */
int convert_pem_to_der( const unsigned char *input, size_t ilen,
                        unsigned char **output, size_t *olen )
{
    int ret;
    const unsigned char *s1, *s2, *end;
    size_t len = 0;
    unsigned char *inpCpy;

    // ensure last byte of input is NULL so that strstr knows where to stop 
    inpCpy = calloc(1, ilen + 1);
    memcpy(inpCpy, input, ilen);
    end = inpCpy + ilen;

    s1 = (unsigned char *) strstr( (const char *) inpCpy, "-----BEGIN" );
    if( s1 == NULL ){
    	ret = -1;
    	goto out;
    }

    s2 = (unsigned char *) strstr( (const char *) inpCpy, "-----END" );
    if( s2 == NULL ){
    	ret = -1;
    	goto out;
    }

    s1 += 10;
    while( s1 < end && *s1 != '-' )
        s1++;
    while( s1 < end && *s1 == '-' )
        s1++;
    if( *s1 == '\r' ) s1++;
    if( *s1 == '\n' ) s1++;

    if( s2 <= s1 || s2 > end ) {
    	ret = -1;
    	goto out;
    }
        

    ret = mbedtls_base64_decode( NULL, 0, &len, (const unsigned char *) s1, s2 - s1 );
    if( ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER ){
    	prlog(PR_ERR, "ERROR: Failed to parse, found invalid character while converting from PEM into DER, mbedtls error #%d\n", ret);
    	goto out;
    }
    // free this ouside of function
    *output = malloc(len);
    if (!*output) {
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		ret = ALLOC_FAIL;
		goto out;
	}
	*olen = len;
    
    if( ( ret = mbedtls_base64_decode( *output, len, &len, (const unsigned char *) s1,
                               s2 - s1 ) ) != 0 )
    {
    	prlog(PR_ERR, "ERROR: Failed to parse, finished %zd/%ld bytes from PEM into DER, mbedtls error #%d\n", len, s2-s1, ret);
    	goto out;

    }

out:
	free(inpCpy);

    return( ret );
}

#ifndef NO_CRYPTO
/*
 *given a data buffer it generates the desired hash 
 *@param data, data to be hashed
 *@param size , length of buff
 *@param hashFunct, mbedtls_md_type, message digest type
 *@param outBuff, the resulting hash, NOTE: REMEMBER TO UNALLOC THIS MEMORY
 *@param outBuffSize, should be alg->size
 *@return SUCCESS or err number 
 */
int toHash(const unsigned char* data, size_t size, int hashFunct, unsigned char** outHash, size_t* outHashSize)
{	
	const mbedtls_md_info_t *md_info;
	mbedtls_md_context_t ctx;
	int rc;
	
	
	md_info = mbedtls_md_info_from_type(hashFunct);

	mbedtls_md_init(&ctx);

	rc = mbedtls_md_setup(&ctx, md_info, 0);
	if (rc) {
		prlog(PR_ERR, "ERROR: Could not setup hashing environment mbedtls err #%d\n", rc);
		goto out;
	}

	rc = mbedtls_md_starts(&ctx);
	if (rc) {
		prlog(PR_ERR, "ERROR: Starting hashing context failed mbedtls err #%d\n", rc);
		goto out;
	}
	prlog(PR_INFO, "Creating %s hash of %zd bytes of data, result will be %d bytes\n", md_info->name, size, md_info->size);
	rc = mbedtls_md_update(&ctx, data, size);
	if (rc) {
		prlog(PR_ERR, "ERROR: Failed to add %zd bytes of data to hashing context failed mbedtls err #%d\n", size, rc);
		goto out;
	}

	*outHash = calloc(1, md_info->size);
	if (!*outHash){
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;
	}
	rc = mbedtls_md_finish(&ctx, *outHash);
	if (rc) {
		prlog(PR_ERR, "ERROR: Generation hash failed mbedtls err #%d\n", rc);
		free(*outHash);
		*outHash = NULL;
		goto out;
	}

	*outHashSize = md_info->size;
	if (verbose){ 
		printf("Hash generation successful, %s: ", md_info->name);
		printHex(*outHash, *outHashSize);
	}
	rc = SUCCESS;

out:
	mbedtls_md_free(&ctx);
	return rc;
}

static int allocateMoreMemory(unsigned char **start, size_t *size, unsigned char **ptr){
	unsigned char *newStart = NULL, *newPtr = NULL;
	size_t currentlyWritten;
	// new buffer is double the size of the old buffer, cant realloc bc new data must be infront of old data
	newStart = calloc(1, *size*2);
	if (!newStart){
		prlog(PR_ERR, "ERROR: failed to allocate %zd bytes more memory\n", *size * 2);
		return ALLOC_FAIL;
	} 
	// number of bytes to copy is the number of bytes between ptr and the end of the buffer
	currentlyWritten = (*start) + (*size) - (*ptr);
	// new buffer will be the same as the old one only there will be 'size' number of bytes padded to the front, 
	// remember we are writing the buffer from highest memory address to lowest so new data will go in the front
	// thank you mbedtls! >:(
	newPtr = newStart + (*size) + ((*ptr) - (*start));
	memcpy(newPtr, *ptr, currentlyWritten);
	// free old buffer
	free(*start);
	// update values
	*start = newStart;
	*ptr = newPtr;
	*size = 2 * (*size);
	return SUCCESS;
}

/*
 *A general way to add data to the pkcs7 buffer from a given tag (data type)
 *@param start, start of the pkcs7 data buffer
 *@param size, size allocated to start
 *@param ptr, points to the current location of where the data has been written to. memory from start to pointer should be unused. REMEMBER mbedtls writes their buffers from the end of a buffer to the start
 *@param tag, the type of data that is trying to be written, not necessarily the same tag that will be added to the pkcs7
 *@param value, the new data to be added to the pkcs7
 *@param valueSize, the length of the new value
 *@param param, extra argument if adding algorthm identifier (tag = MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC) to show the size of the buffer, often 0
*/
static int setPKCS7Data(unsigned char **start, size_t *size, unsigned char **ptr, int tag, const void* value, size_t valueSize, int param) {
	int rc; 
	// pointer for current spot in data, in case it fails but ptr changes
	unsigned char *ptrTmp = *ptr; 
	do {
	// do funtion for tag
		if (tag == MBEDTLS_ASN1_INTEGER)
			rc = mbedtls_asn1_write_int(ptr, *start, *(int *) value);
		// if 0x30 or 0xA0then write length and write tag in next iteration
		else if (tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)  	|| 
		tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED)		||
		tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)) {
			rc = mbedtls_asn1_write_len(ptr, *start, valueSize);
				if (rc >= 0)
					rc = mbedtls_asn1_write_tag(ptr, *start,  tag);
		}
		// for OID + constructed|sequence + len
		else if (tag == (MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC))
			rc = mbedtls_asn1_write_algorithm_identifier(ptr, *start, value, valueSize, param);
		// for just oid 
		else if (tag == MBEDTLS_ASN1_OID) {
			rc = mbedtls_asn1_write_oid(ptr, *start, value, valueSize);
			if (rc >= 0) {
				rc = mbedtls_asn1_write_len(ptr, *start, ptrTmp - *ptr); 
				if (rc >= 0 ) {
					rc = mbedtls_asn1_write_tag(ptr, *start,(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) );
				}
			}
		}
		// for signature
		else if (tag == MBEDTLS_ASN1_OCTET_STRING) {
			rc = mbedtls_asn1_write_octet_string(ptr, *start, value, valueSize);
		}
		// for raw data, idk kinda makes sense bit string = raw data you know maybe...
		else if (tag == MBEDTLS_ASN1_BIT_STRING)
			rc = mbedtls_asn1_write_raw_buffer(ptr, *start, value, valueSize);
		// for long integers of any length, ex:serial #, I am getting creative with these combos!
		else if (tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_INTEGER))
			rc = mbedtls_asn1_write_tagged_string(ptr, *start, MBEDTLS_ASN1_INTEGER, value, valueSize);
		// if setting data failed, allocate more data
		if (rc == MBEDTLS_ERR_ASN1_BUF_TOO_SMALL) {
			*ptr = ptrTmp; // set ptr back to old one, could have changed when funciton failed
			
			if (allocateMoreMemory(start, size, ptr)){
				prlog(PR_ERR, "Failed to allocate more memory (total %zd) for PKCS7\n", *size * 2);
				return FILE_WRITE_FAIL;
			}
			// reset ptrTmp because ptr was allacated to some other place in memory
			ptrTmp = *ptr; 
		}
		else if (rc < 0) {
			prlog(PR_ERR, "ERROR: Issue with writing data for tag %d, mbedtls error #%d\n", tag, rc);
			return FILE_WRITE_FAIL;
		}

	}
	while (rc == MBEDTLS_ERR_ASN1_BUF_TOO_SMALL);

	return SUCCESS;
}





static int setSignature(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info, 
			mbedtls_x509_crt *pub, unsigned char *priv, size_t privSize) {
	int rc;
	size_t sigSize, hashSize, sigSizeBits;
	unsigned char *hash = NULL, *signature = NULL;
	char *sigType = NULL;
	mbedtls_pk_context *privKey;
	

	privKey = malloc(sizeof(*privKey));
	if (!privKey){
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		return ALLOC_FAIL;
	}
	mbedtls_pk_init(privKey);
	// make sure private key parses into private key format
	rc = mbedtls_pk_parse_key(privKey, priv, privSize, NULL, 0);
	if (rc) {
		prlog(PR_ERR, "ERROR: Failed to get context of private key, mbedtls error #%d\n", rc);
		goto out;
	}
	// make sure private key is matched with public key
	rc = mbedtls_pk_check_pair(&(pub->pk), privKey);
	if (rc) {
		prlog(PR_ERR, "Public and private key are not matched, mbedtls err#%d\n", rc);
		goto out;
	}
	// make sure private key is RSA, otherwise quit
	sigType = (char *) privKey->pk_info->name;
	if (strcmp(sigType, "RSA")) {
		rc = CERT_FAIL;
		prlog(PR_ERR, "ERROR: Key is of type %s expected RSA\n", sigType);
		goto out;
	}
	// get size of RSA signature, ex 2048, 4096 ...
	sigSizeBits = privKey->pk_info->get_bitlen(privKey->pk_ctx);

	// at this point we know pub and priv are valid, now we need the data to sign
	rc = toHash(pkcs7Info->newData, pkcs7Info->newDataSize, pkcs7Info->hashFunct, &hash, &hashSize);
	if (rc) {
		prlog(PR_ERR, "ERROR: Failed to generate hash of new data for signing\n");
		goto out;
	}
	signature = malloc(sigSizeBits/8);
	if (!signature){
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;
	}

	// sign
	if (verbose)
		printf("Signing digest of %zd bytes with %s into %zd bits \n", hashSize, sigType, sigSizeBits);
	rc = mbedtls_pk_sign(privKey, pkcs7Info->hashFunct, hash, 0, signature, &sigSize, 0, NULL);
	if (rc) {
		prlog(PR_ERR, "Failed to generate signature, mbedtls err #%d\n", rc);
		goto out;
	}
	rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OCTET_STRING, signature, sigSize, 0);
	if (rc) {
		prlog(PR_ERR, "Failed to add signature to PKCS7 (signature generation was successful however)\n");
	}
out:
	mbedtls_pk_free(privKey);
	if (privKey) free(privKey);
	if (hash) free(hash);
	if (signature) free(signature);
	return rc;

}

static int setAlgorithmIDs(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info, mbedtls_x509_crt *pub, unsigned char *priv, size_t privSize) {
	int rc;
	char *sigType = NULL;
	
	// if the private key already holds signature (see definition of pkcs7Info.keys)
	// then just write the signature, no generation is needed
	if (pkcs7Info->alreadySignedFlag) {
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OCTET_STRING, priv, privSize, 0);
		if (rc)
			prlog(PR_ERR, "Failed to add signature to PKCS7\n");
	}
	else
		rc = setSignature(start, size, ptr, pkcs7Info, pub, priv, privSize);
	if (!rc){
		// make sure it is rsa encryption, that is all we support right now
		sigType = (char *) pub->pk.pk_info->name;
		if (strcmp(sigType, "RSA")) {
			rc = CERT_FAIL;
			prlog(PR_ERR, "ERROR: Public Key is of type %s expected RSA\n", sigType);
			return rc;
		}
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC, (void *) MBEDTLS_OID_PKCS1_RSA, strlen(MBEDTLS_OID_PKCS1_RSA), 0);
		if (!rc) {
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC, (void *) pkcs7Info->hashFunctOID, strlen(pkcs7Info->hashFunctOID), 0);
		}
	}
	return rc;
}

static int setSignerCertData(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info, mbedtls_x509_crt *pub, unsigned char *priv, size_t privSize) {
	int rc, signedInfoVersion = 1; 
	size_t bytesWrittenInStep, currentlyUsedBytes;
	rc = setAlgorithmIDs(start, size, ptr, pkcs7Info, pub, priv, privSize);
	if (!rc) {
		// add serial
		currentlyUsedBytes = *size - (*ptr - *start);
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_INTEGER, pub->serial.p, pub->serial.len, 0);
		if (!rc) {
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_BIT_STRING, pub->issuer_raw.p, pub->issuer_raw.len, 0);
			if (!rc){
				// add info
				bytesWrittenInStep = *size - (*ptr - *start) - currentlyUsedBytes;
				rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, NULL, bytesWrittenInStep, 0);
				if (!rc) {
					// add signed info version, see https:// tools.ietf.org/html/rfc2315 section 9.2
					rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_INTEGER, &signedInfoVersion, sizeof(signedInfoVersion), 0);
				}
				if (rc) {
						prlog(PR_ERR, "ERROR: Failed to add owner flag to Signer Info of PKCS7\n");
					}
			}
			else prlog(PR_ERR, "ERROR: Failed to add issuer data to Signer info of PKCS7\n");
		}
		else prlog(PR_ERR, "ERROR: Failed to add certificate serial number to Signer info of PKCS7\n");
	}
	return rc;
}

static int setSignerDataForEachSigner(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info){
	int rc;
	size_t bytesWrittenInStep, currentlyUsedBytes;
	mbedtls_x509_crt *x509 = NULL;
	// if no signers than quit
	if (pkcs7Info->keyPairs < 1) {
		rc = ARG_PARSE_FAIL;
		prlog(PR_ERR, "ERROR: No keys given to sign with\n");
	}
	for(int i = 0; i < pkcs7Info->keyPairs ; i++) {
		x509 = malloc(sizeof(*x509));
		if (!x509){
			prlog(PR_ERR, "ERROR: failed to allocate memory\n");
			rc = ALLOC_FAIL;
			goto out;
		}
		mbedtls_x509_crt_init(x509);
		// puts cert data into x509_Crt struct and returns number of failed parses
		rc = mbedtls_x509_crt_parse(x509, pkcs7Info->crts[i], pkcs7Info->crtSizes[i]); 
		if (rc) {
			prlog(PR_ERR, "ERROR: While extracting signer info, parsing x509 failed with MBEDTLS exit code: %d \n", rc);
			goto out;
		}
		currentlyUsedBytes = *size - (*ptr - *start);
		rc = setSignerCertData(start, size, ptr, pkcs7Info, x509, pkcs7Info->keys[i], pkcs7Info->keySizes[i]);
		if (rc) goto out;
		bytesWrittenInStep = *size - (*ptr - *start) - currentlyUsedBytes;
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, NULL, bytesWrittenInStep, 0);
		if (rc) {
			prlog(PR_ERR,"ERROR: Failed to add header seqeuence header for signer data\n");
			goto out;
		}

		mbedtls_x509_crt_free(x509);
		if (x509) free(x509);
		x509 = NULL;
	}
	out:
	if (x509) mbedtls_x509_crt_free(x509);
	if (x509) free(x509);
	return rc;
}

static int setSignersData(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info) {
	int rc;
	size_t bytesWrittenInStep, currentlyUsedBytes;
	currentlyUsedBytes = *size - (*ptr - *start);
	rc = setSignerDataForEachSigner(start, size, ptr, pkcs7Info);
	if (!rc) {
		bytesWrittenInStep = *size - (*ptr - *start) - currentlyUsedBytes;
		// add 0x31 with length to end of signers info 
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, NULL, bytesWrittenInStep, 0); 
	}
	if (rc) {
		prlog(PR_ERR, "ERROR: Failed to add signers data header info to PKCS7\n");
	}
	return rc;
}

static int setSignerCertRaw(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info) {
	int rc;
	size_t bytesWrittenInStep, currentlyUsedBytes;
	rc = setSignersData(start, size, ptr, pkcs7Info);

	if (!rc) {
		currentlyUsedBytes = *size - (*ptr - *start);
		for (int i =0; i < pkcs7Info->keyPairs; i++) {
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_BIT_STRING, pkcs7Info->crts[i], pkcs7Info->crtSizes[i], 0);
			if (rc) break;
		}
		if (!rc){
			bytesWrittenInStep = *size - (*ptr - *start) - currentlyUsedBytes;
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED, NULL, bytesWrittenInStep, 0);
		}
		if (rc) {
			prlog(PR_ERR, "ERROR: Failed to add raw signing certificate to PKCS7\n");
		}
	}
	return rc;
}

static int setSignedDataOID(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info) {
	int rc;
	rc = setSignerCertRaw(start, size, ptr, pkcs7Info);
	if (!rc) {
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OID , (void *)MBEDTLS_OID_PKCS7_DATA, strlen(MBEDTLS_OID_PKCS7_DATA), 0);
		if (rc){
			prlog(PR_ERR, "ERROR: Failed to add OID, PKCS7_DATA, for Signed Data of PKCS7\n");
		}
	}
	return rc;
}

static int setAlgoID(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info) {
	int rc;
	size_t bytesWrittenInStep, currentlyUsedBytes;
	rc = setSignedDataOID(start, size, ptr, pkcs7Info);
	
	if (!rc){
		currentlyUsedBytes = *size - (*ptr - *start);

		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC, (void *) pkcs7Info->hashFunctOID, strlen(pkcs7Info->hashFunctOID), 0);
		if (!rc){
		// bytes in step = new currently written bytes - old currently written bytes
			bytesWrittenInStep = *size - (*ptr - *start) - currentlyUsedBytes; 
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET, NULL, bytesWrittenInStep, 0);
		}
		if (rc) 
		prlog(PR_ERR, "ERROR: Failed to add algorithm ID to PKCS7\n");
	}
	
	return rc;
}

static int setVersion(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info){
	int rc;
	int version = 1;
	// for now only version 1
	rc = setAlgoID(start, size, ptr, pkcs7Info);
	if (!rc){
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_INTEGER, &version, sizeof(version), 0);
		if (rc)
			prlog(PR_ERR, "ERROR: Failed to add version to PKCS7\n");
	}
	
	return rc;
}

static int setSignedData(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info){
	int rc;
	size_t currentlyUsedBytes;
	rc = setVersion(start, size, ptr, pkcs7Info);
	if (!rc){
		currentlyUsedBytes = *size - (*ptr - *start);
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE, NULL, currentlyUsedBytes, 0);
		if (rc)
			prlog(PR_ERR,"ERROR: Failed to add signed data's SEQUENCE header to PKCS7\n");
	}

	return rc;

}

static int setPKCS7OID(unsigned char **start, size_t *size, unsigned char **ptr, PKCS7Info *pkcs7Info){
	int rc;
	size_t currentlyUsedBytes;

	rc = setSignedData(start, size, ptr, pkcs7Info);
	
	if (!rc){
		currentlyUsedBytes = *size - (*ptr - *start);
		rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED, NULL, currentlyUsedBytes, 0);
		if (!rc){
			currentlyUsedBytes = *size - (*ptr - *start);
			rc = setPKCS7Data(start, size, ptr, MBEDTLS_ASN1_OID | MBEDTLS_ASN1_CONTEXT_SPECIFIC, (void *) MBEDTLS_OID_PKCS7_SIGNED_DATA, strlen(MBEDTLS_OID_PKCS7_SIGNED_DATA), currentlyUsedBytes );	
		}
		if (rc)
			prlog(PR_ERR, "ERROR: Failed to add PKCS7 OID/SEQUENCE header to PKCS7\n");
	}
	return rc;
}

static int toPKCS7(unsigned char **pkcs7, size_t *pkcs7Size, const char** crtFiles,
		   int keyPairs, int hashFunct, PKCS7Info *info) 
{
	unsigned char *crtPEM = NULL,**crts = NULL,*pkcs7Buff = NULL;
	unsigned char *ptr;
	const char *hashFunctOID;
	size_t crtSizePEM,*crtSizes = NULL, pkcs7BuffSize, whiteSpace, oidLen; 
	int rc;

	crts = calloc (1, sizeof(unsigned char*) * keyPairs);
	crtSizes = calloc(1, sizeof(size_t) * keyPairs);
	if (!crts || !crtSizes) {
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;
	}
	for (int i = 0; i < keyPairs; i++) {
		// get data from public keys
		crtPEM = (unsigned char *)getDataFromFile(crtFiles[i], &crtSizePEM);
		if (!crtPEM) {
			prlog(PR_ERR, "ERROR: failed to get data from pub key file %s\n", crtFiles[i]);
			rc = INVALID_FILE;
			goto out;
		}
		// get der format of that crt
		rc = convert_pem_to_der(crtPEM, crtSizePEM, (unsigned char **) &crts[i], &crtSizes[i]);
		if (rc) {
			prlog(PR_ERR, "Conversion for %s from PEM to DER failed\n", crtFiles[i]);
			goto out;
		}
		if (crtPEM) free(crtPEM);
		crtPEM = NULL;
	}
	// get hashFunct OID
	if (hashFunct < MBEDTLS_MD_NONE || hashFunct > MBEDTLS_MD_RIPEMD160) {
		prlog(PR_ERR, "ERROR: Invalid hash function %d, see mbedtls_md_type_t\n", hashFunct);
		rc = HASH_FAIL;
		goto out;
	}
	rc = mbedtls_oid_get_oid_by_md( hashFunct, (const char **)&hashFunctOID, &oidLen);
	if (rc) {
		prlog(PR_ERR, "Message Digest value %d could not be converted to an OID, mbedtls err #%d\n",hashFunct, rc);
	}

	info->crts = (unsigned char **)crts;
	info->crtSizes = crtSizes;
	info->keyPairs = keyPairs;
	info->hashFunct = hashFunct;
	info->hashFunctOID = hashFunctOID;

	prlog(PR_INFO, "Generating Pkcs7 with %d pair(s) of signers...\n", keyPairs);
	
	// buffer size for pkcs7 will grow exponentially 2^n depending on space needed
	pkcs7BuffSize = 2;
	pkcs7Buff = malloc(pkcs7BuffSize);
	if (!pkcs7Buff){
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;
	}
	// set ptr to the end of the buffer, mbedtls functions write backwards 
	ptr = pkcs7Buff + pkcs7BuffSize;	
	// this will call all other functions
	rc = setPKCS7OID((unsigned char **) &pkcs7Buff, &pkcs7BuffSize, &ptr, info);
	if (rc){
		prlog(PR_ERR, "Failed to generate PKCS7\n");
		goto out;
	}
	// trim pkcs7
	whiteSpace = getLeadingWhitespace(pkcs7Buff, pkcs7BuffSize);
	prlog(PR_INFO, "Trimmed PKCS7 of buffer size: %zd to actual size = %zd\n", pkcs7BuffSize, pkcs7BuffSize - whiteSpace);
	pkcs7BuffSize -= whiteSpace; // factor off leading whitespace from pkcs7

	// copy into new buffer with only necessary allocated memory
	*pkcs7Size = pkcs7BuffSize;
	*pkcs7 = malloc(*pkcs7Size);
	if (!*pkcs7){
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;
	}
	memcpy(*pkcs7, pkcs7Buff + whiteSpace, *pkcs7Size);

out:
	if (crtPEM) free(crtPEM);
	for (int i = 0; i < keyPairs; i++) {
		if (crts[i]) free(crts[i]);
	}
	if (crts) free(crts);
	if (crtSizes) free(crtSizes);
	if (pkcs7Buff) free(pkcs7Buff);

	return rc;
}

/*
 *generates a PKCS7 and create signature with private and public keys
 *@param pkcs7, the resulting PKCS7, newData not appended, NOTE: REMEMBER TO UNALLOC THIS MEMORY
 *@param pkcs7Size, the length of pkcs7
 *@param newData, data to be added to be used in digest
 *@param dataSize , length of newData
 *@param crtFiles, array of file paths to public keys to sign with(PEM)
 *@param keyFiles, array of file paths to private keys to sign with
 *@param keyPairs, array length of key/crtFiles
 *@param hashFunct, hash function to use in digest, see mbedtls_md_type_t for values in mbedtls/md.h
 *@return SUCCESS or err number 
 */
int to_pkcs7_generate_signature(unsigned char **pkcs7, size_t *pkcs7Size, const unsigned char *newData, size_t newDataSize, 
	const char** crtFiles, const char** keyFiles,  int keyPairs, int hashFunct)
{
	unsigned char *keyPEM = NULL, **keys = NULL;
	size_t keySizePEM,  *keySizes = NULL;
	int rc;
	PKCS7Info info;
	// if no keys given
	if (keyPairs == 0) {
		prlog(PR_ERR, "ERROR: missing private key / certificate... use -k <privateKeyFile> -c <certificateFile>\n");
		rc = ARG_PARSE_FAIL;
		goto out;
	}
	keys = calloc(1, sizeof(unsigned char*) * keyPairs);
	keySizes = calloc(1, sizeof(size_t) * keyPairs);
	if (!keys || !keySizes) {
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;	
	}

	for (int i = 0; i < keyPairs; i++) {
		// get data of private keys
		keyPEM = (unsigned char *)getDataFromFile(keyFiles[i], &keySizePEM);

		if (!keyPEM) {
			prlog(PR_ERR, "ERROR: failed to get data from priv key file %s\n", keyFiles[i]);
			rc = INVALID_FILE;
			goto out;
		}
		rc = convert_pem_to_der(keyPEM, keySizePEM, (unsigned char **) &keys[i], &keySizes[i]);
		if (rc) {
			prlog(PR_ERR, "Conversion for %s from PEM to DER failed\n", keyFiles[i]);
			goto out;
		}
		if (keyPEM) free(keyPEM);
		keyPEM = NULL;
	}
	
	info.keys = (unsigned char **)keys;
	info.keySizes = keySizes;
	info.keyPairs = keyPairs;
	info.newData = newData;
	info.newDataSize = newDataSize;
	info.alreadySignedFlag = 0;

	rc = toPKCS7(pkcs7, pkcs7Size, crtFiles, keyPairs, hashFunct, &info);
	if (rc)
		goto out;

	if (verbose){
		printf( "PKCS7 generation successful...\n");
	}
out:
	if (keyPEM) free(keyPEM);
	for (int i = 0; i < keyPairs; i++) {
		if (keys[i]) free(keys[i]);
	}
	if (keys) free (keys);
	if (keySizes) free(keySizes);

	return rc;
}

/*
 *generates a PKCS7 with given signed data
 *@param pkcs7, the resulting PKCS7, newData not appended, NOTE: REMEMBER TO UNALLOC THIS MEMORY
 *@param pkcs7Size, the length of pkcs7
 *@param newData, data to be added to be used in digest
 *@param dataSize , length of newData
 *@param crtFiles, array of file paths to public keys that were used in signing with(PEM)
 *@param sigFiles, array of file paths to raw signed data files 
 *@param keyPairs, array length of crt/signatures
 *@param hashFunct, hash function to use in digest, see mbedtls_md_type_t for values in mbedtls/md.h
 *@return SUCCESS or err number 
 */
int to_pkcs7_already_signed_data(unsigned char **pkcs7, size_t *pkcs7Size, const unsigned char *newData, size_t newDataSize, 
	const char** crtFiles, const char** sigFiles,  int keyPairs, int hashFunct)
{
	char **sigs = NULL;
	size_t  *sig_sizes = NULL;
	int rc;
	PKCS7Info info;
	// if no keys given
	if (keyPairs == 0) {
		prlog(PR_ERR, "ERROR: missing signature / certificate pairs... use -s <signedDataFile> -c <certificateFile>\n");
		rc = ARG_PARSE_FAIL;
		goto out;
	}
	sigs = calloc(1, sizeof(char*) * keyPairs);
	sig_sizes = calloc(1, sizeof(size_t) * keyPairs);
	if (!sigs || !sig_sizes) {
		prlog(PR_ERR, "ERROR: failed to allocate memory\n");
		rc = ALLOC_FAIL;
		goto out;	
	}

	for (int i = 0; i < keyPairs; i++) {
		// get data from signature files
		sigs[i] = getDataFromFile(sigFiles[i], &sig_sizes[i]);

		if (!sigs[i]) {
			prlog(PR_ERR, "ERROR: failed to get data from signature file %s\n", sigFiles[i]);
			rc = INVALID_FILE;
			goto out;
		}
	}
	
	info.keys = (unsigned char **)sigs;
	info.keySizes = sig_sizes;
	info.keyPairs = keyPairs;
	info.newData = newData;
	info.newDataSize = newDataSize;
	info.alreadySignedFlag = 1;

	rc = toPKCS7(pkcs7, pkcs7Size, crtFiles, keyPairs, hashFunct, &info);
	if (rc)
		goto out;

	if (verbose){
		printf( "PKCS7 generation successful...\n");
	}
out:
	for (int i = 0; i < keyPairs; i++) {
		if (sigs[i]) free(sigs[i]);
	}
	if (sigs) free (sigs);
	if (sig_sizes) free(sig_sizes);
	return rc;
}
#endif

/*
 * Copyright (C) 2011-2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <stdio.h>
#include <string.h>
#include <assert.h>

# include <unistd.h>
# include <pwd.h>
# define MAX_PATH FILENAME_MAX

#include "sgx_urts.h"
#include "App.h"
#include "Enclave_u.h"
#include "Seal_u.h"

#define SEAL_FILENAME             "Seal.signed.so"
#define SEALED_KEY_FILE_NAME     "sealed_key.bin"
#define TOKEN_FILENAME            "enclave.token"

/* Global EID shared by multiple threads */
sgx_enclave_id_t global_eid = 0;

typedef struct _sgx_errlist_t {
    sgx_status_t err;
    const char *msg;
    const char *sug; /* Suggestion */
} sgx_errlist_t;

/* Error code returned by sgx_create_enclave */
static sgx_errlist_t sgx_errlist[] = {
    {
        SGX_ERROR_UNEXPECTED,
        "Unexpected error occurred.",
        NULL
    },
    {
        SGX_ERROR_INVALID_PARAMETER,
        "Invalid parameter.",
        NULL
    },
    {
        SGX_ERROR_OUT_OF_MEMORY,
        "Out of memory.",
        NULL
    },
    {
        SGX_ERROR_ENCLAVE_LOST,
        "Power transition occurred.",
        "Please refer to the sample \"PowerTransition\" for details."
    },
    {
        SGX_ERROR_INVALID_ENCLAVE,
        "Invalid enclave image.",
        NULL
    },
    {
        SGX_ERROR_INVALID_ENCLAVE_ID,
        "Invalid enclave identification.",
        NULL
    },
    {
        SGX_ERROR_INVALID_SIGNATURE,
        "Invalid enclave signature.",
        NULL
    },
    {
        SGX_ERROR_OUT_OF_EPC,
        "Out of EPC memory.",
        NULL
    },
    {
        SGX_ERROR_NO_DEVICE,
        "Invalid Intel(R) SGX device.",
        "Please make sure Intel(R) SGX module is enabled in the BIOS, and install Intel(R) SGX driver afterwards."
    },
    {
        SGX_ERROR_MEMORY_MAP_CONFLICT,
        "Memory map conflicted.",
        NULL
    },
    {
        SGX_ERROR_INVALID_METADATA,
        "Invalid enclave metadata.",
        NULL
    },
    {
        SGX_ERROR_DEVICE_BUSY,
        "Intel(R) SGX device was busy.",
        NULL
    },
    {
        SGX_ERROR_INVALID_VERSION,
        "Enclave version was invalid.",
        NULL
    },
    {
        SGX_ERROR_INVALID_ATTRIBUTE,
        "Enclave was not authorized.",
        NULL
    },
    {
        SGX_ERROR_ENCLAVE_FILE_ACCESS,
        "Can't open enclave file.",
        NULL
    },
    {
        SGX_ERROR_PCL_ENCRYPTED,
        "sgx_create_enclave can't open encrypted enclave.",
        NULL
    },
    {
        SGX_ERROR_PCL_NOT_ENCRYPTED,
        "sgx_create_encrypted_enclave can't open not-encrypted enclave.",
        NULL
    },
    {
        SGX_ERROR_PCL_MAC_MISMATCH,
        "PCL detected invalid section in encrypted enclave.",
        NULL
    },
    {
        SGX_ERROR_PCL_SHA_MISMATCH,
        "PCL sealed key SHA mismatch.",
        NULL
    },
    {
        SGX_ERROR_PCL_GUID_MISMATCH,
        "PCL sealed key GUID mismatch.",
        NULL
    },
};

/* Check error conditions for loading enclave */
void print_error_message(sgx_status_t ret)
{
    size_t idx = 0;
    size_t ttl = sizeof sgx_errlist/sizeof sgx_errlist[0];

    for (idx = 0; idx < ttl; idx++) {
        if(ret == sgx_errlist[idx].err) {
            if(NULL != sgx_errlist[idx].sug)
                printf("Info: %s\n", sgx_errlist[idx].sug);
            printf("Error: %s\n", sgx_errlist[idx].msg);
            break;
        }
    }
    
    if (idx == ttl)
        printf("Error: Unexpected error occurred.\n");
}

/* Initialize the enclave:
 *   Step 1: try to retrieve the launch token saved by last transaction
 *   Step 2: call sgx_create_enclave to initialize an enclave instance
 *   Step 3: save the launch token if it is updated
 */
sgx_status_t  initialize_enclave ( const char *file_name, sgx_enclave_id_t* eid )
{
    char token_path[MAX_PATH] = {'\0'};
    sgx_launch_token_t token = {0};
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    int updated = 0;
    size_t read_num = 0;
    
    /* Step 1: try to retrieve the launch token saved by last transaction 
     *         if there is no token, then create a new one.
     */
    /* try to get the token saved in $HOME */
    const char *home_dir = getpwuid(getuid())->pw_dir;
    
    if (home_dir != NULL && 
        (strlen(home_dir)+strlen("/")+sizeof(TOKEN_FILENAME)+1) <= MAX_PATH) {
        /* compose the token path */
        strncpy(token_path, home_dir, strlen(home_dir));
        strncat(token_path, "/", strlen("/"));
        strncat(token_path, TOKEN_FILENAME, sizeof(TOKEN_FILENAME)+1);
    } else {
        /* if token path is too long or $HOME is NULL */
        strncpy(token_path, TOKEN_FILENAME, sizeof(TOKEN_FILENAME));
    }

    FILE *fp = fopen(token_path, "rb");
    if (fp == NULL && (fp = fopen(token_path, "wb")) == NULL) {
        printf("Warning: Failed to create/open the launch token file \"%s\".\n", token_path);
    }

    if (fp != NULL) {
        /* read the token from saved file */
        read_num = fread(token, 1, sizeof(sgx_launch_token_t), fp);
        if (read_num != 0 && read_num != sizeof(sgx_launch_token_t)) {
            /* if token is invalid, clear the buffer */
            memset(&token, 0x0, sizeof(sgx_launch_token_t));
            printf("Warning: Invalid launch token read from \"%s\".\n", token_path);
        }
    }
    /* Step 2: call sgx_create_enclave to initialize an enclave instance */
    /* Debug Support: set 2nd parameter to 1 */
#ifdef SGX_USE_PCL        
    bool open_seal_enclave = true;
    uint8_t* sealed_blob = NULL;
    FILE *fsealp = fopen(SEALED_KEY_FILE_NAME, "rb");
    size_t sealed_blob_size = 0;
    if(NULL != fsealp)
    {   
        // Read file size:
        fseek(fsealp, 0L, SEEK_END);
        sealed_blob_size = ftell(fsealp);
        fseek(fsealp, 0L, SEEK_SET);
        // Read file into buffer:
        sealed_blob = new uint8_t[sealed_blob_size];
        read_num = fread(sealed_blob, 1, sealed_blob_size, fsealp);
        if ( read_num != sealed_blob_size )
        {
            printf ( "Warning: Failed to read sealed blob.\n" );
        }
        else
        {
            open_seal_enclave = false;
        }
        fclose(fsealp);
    }
    if (true == open_seal_enclave)
    {
        printf ("Open Seal Enclave: %s\n", SEAL_FILENAME );
        sgx_enclave_id_t seal_eid = 0;
        ret = sgx_create_enclave(
            SEAL_FILENAME, 
            SGX_DEBUG_FLAG, 
            &token, 
            &updated, 
            &seal_eid, 
            NULL);
        if (SGX_SUCCESS != ret) 
        {
            print_error_message(ret);
            if(NULL != fp)
            {
               fclose(fp);
            }
            return ret;
        }        
        ret = ecall_get_sealed_blob_size(seal_eid, &sealed_blob_size);
        if (ret != SGX_SUCCESS || UINT32_MAX == sealed_blob_size)
        {
            printf("ecall_get_sealed_blob_size: ret = %d, sealed_blob_size = %ld\n", ret, sealed_blob_size);
            sgx_destroy_enclave(seal_eid);
            return ret;
        }
        //printf("ecall_get_sealed_blob_size: ret = %d, sealed_blob_size = %ld\n", ret, sealed_blob_size);
        sealed_blob = new uint8_t[sealed_blob_size];
        sgx_status_t gret = SGX_ERROR_UNEXPECTED;
        ret = ecall_generate_sealed_blob(seal_eid, &gret, sealed_blob, sealed_blob_size);
        if ((SGX_SUCCESS != ret) || (SGX_SUCCESS != gret)) 
        {
            printf("ecall_generate_sealed_blob: ret = %d, gret = 0x%x\n", ret, gret);
            sgx_destroy_enclave(seal_eid);
            delete sealed_blob;
            return ret;
        }
        sgx_destroy_enclave(seal_eid);
        fsealp = fopen(SEALED_KEY_FILE_NAME, "wb");
        if(NULL != fsealp)
        {
            fwrite(sealed_blob, 1, sealed_blob_size, fsealp);
            fclose(fsealp);
        }
    }
    // Load the PCL protected Enclave:
    ret = sgx_create_encrypted_enclave(file_name, SGX_DEBUG_FLAG, &token, &updated, eid, NULL, sealed_blob);
    delete sealed_blob;
#else  // SGX_USE_PCL
    ret = sgx_create_enclave(file_name, SGX_DEBUG_FLAG, &token, &updated, eid, NULL);
#endif // SGX_USE_PCL
    if (ret != SGX_SUCCESS) {
        print_error_message(ret);
        if (fp != NULL) fclose(fp);
        return ret;
    }

    /* Step 3: save the launch token if it is updated */
    if (updated == FALSE || fp == NULL) {
        /* if the token is not updated, or file handler is invalid, do not perform saving */
        if (fp != NULL) fclose(fp);
        return SGX_SUCCESS;
    }

    /* reopen the file with write capablity */
    fp = freopen(token_path, "wb", fp);
    if (fp == NULL) return SGX_SUCCESS;
    size_t write_num = fwrite(token, 1, sizeof(sgx_launch_token_t), fp);
    if (write_num != sizeof(sgx_launch_token_t))
        printf("Warning: Failed to save launch token to \"%s\".\n", token_path);
    fclose(fp);
    return SGX_SUCCESS;
}

/* OCall functions */
void ocall_print_string(const char *str)
{
    /* Proxy/Bridge will check the length and null-terminate 
     * the input string to prevent buffer overflow. 
     */
    printf("%s", str);
}


/* Application entry */
int SGX_CDECL main(int argc, char *argv[])
{
    (void)(argc);
    (void)(argv);


    /* Initialize the enclave */
    if ( initialize_enclave ( ENCLAVE_FILENAME, &global_eid ) < 0 ){
        return -1; 
    }
 
    /* Utilize edger8r attributes */
    edger8r_array_attributes();
    edger8r_pointer_attributes();
    edger8r_type_attributes();
    edger8r_function_attributes();
    
    /* Utilize trusted libraries */
    ecall_libc_functions();
    ecall_libcxx_functions();
    ecall_thread_functions();

    /* Destroy the enclave */
    sgx_destroy_enclave(global_eid);
    
    printf("Info: SampleEnclave successfully returned.\n");

    return 0;
}


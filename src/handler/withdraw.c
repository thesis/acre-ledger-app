/*****************************************************************************
 *   Ledger App Acre.
 *   (c) 2024 Ledger SAS.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include <stdint.h>

#include "boilerplate/io.h"
#include "boilerplate/dispatcher.h"
#include "boilerplate/sw.h"
#include "../common/bip32.h"
#include "../commands.h"
#include "../constants.h"
#include "../crypto.h"
#include "../common/read.h"
#include "../ui/display.h"
#include "../ui/menu.h"
#include "lib/get_merkle_leaf_element.h"
#include "../common/script.h"

#include "handlers.h"
#include "../swap/handle_check_address.h"
#include "crypto.h"
#include "../common/script.h"

#define DATA_CHUNK_INDEX_1    5
#define DATA_CHUNK_INDEX_2    10
#define CHUNK_SIZE_IN_BYTES   64
#define ADDRESS_SIZE_IN_BYTES 20
#define ADDRESS_SIZE_IN_CHARS 40
#define AMOUNT_SIZE_IN_BYTES  8
#define AMOUNT_SIZE_IN_CHARS  50
#define CHUNK_SECOND_PART     32
#define KECCAK_256_HASH_SIZE  32
#define FIELD_SIZE            32
#define MAX_TICKER_LEN        5

// Constants for hash computation

static unsigned char const BSM_SIGN_MAGIC[] = {'\x18', 'B', 'i', 't', 'c', 'o', 'i', 'n', ' ',
                                               'S',    'i', 'g', 'n', 'e', 'd', ' ', 'M', 'e',
                                               's',    's', 'a', 'g', 'e', ':', '\n'};

#define COIN_VARIANT_ACRE         1
#define COIN_VARIANT_ACRE_TESTNET 2

#if !defined(COIN_VARIANT)
#error "COIN_VARIANT is not defined"
#elif COIN_VARIANT == COIN_VARIANT_ACRE
// Mainnet hash
// Mainnet Chain ID - 1 (0x01)
static const uint8_t abi_encoded_chain_id[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
#elif COIN_VARIANT == COIN_VARIANT_ACRE_TESTNET
// Testnet hash
// Sepolia Chain ID - 11155111 (0xaa36a7)
static const uint8_t abi_encoded_chain_id[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x36, 0xa7};
#else
#error "Unsupported COIN_VARIANT value"
#endif

static const uint8_t domain_separator_typehash[32] = {
    0x47, 0xe7, 0x95, 0x34, 0xa2, 0x45, 0x95, 0x2e, 0x8b, 0x16, 0x89, 0x3a, 0x33, 0x6b, 0x85, 0xa3,
    0xd9, 0xea, 0x9f, 0xa8, 0xc5, 0x73, 0xf3, 0xd8, 0x03, 0xaf, 0xb9, 0x2a, 0x79, 0x46, 0x92, 0x18};

static const uint8_t safe_tx_typehash[32] = {
    0xbb, 0x83, 0x10, 0xd4, 0x86, 0x36, 0x8d, 0xb6, 0xbd, 0x6f, 0x84, 0x94, 0x02, 0xfd, 0xd7, 0x3a,
    0xd5, 0x3d, 0x31, 0x6b, 0x5a, 0x4b, 0x26, 0x44, 0xad, 0x6e, 0xfe, 0x0f, 0x94, 0x12, 0x86, 0xd8};

/**
 * @brief Checks if the provided address matches the address derived from the given BIP32 path.
 *
 * This function derives a compressed public key from the provided BIP32 path and then generates
 * an address from this public key. It compares the generated address with the provided address
 * to check if they match.
 *
 * @param bip32_path A pointer to an array containing the BIP32 path.
 * @param bip32_path_len The length of the BIP32 path array.
 * @param address_to_check A pointer to the address string to be checked.
 * @param address_to_check_len The length of the address string to be checked.
 * @param address_type The type of address to generate (e.g., P2PKH, P2SH, SegWit).
 *
 * @return true if the generated address matches the provided address, false otherwise.
 */
static bool check_address(uint32_t* bip32_path,
                          uint8_t bip32_path_len,
                          char* address_to_check,
                          uint8_t address_to_check_len,
                          uint8_t address_type) {
    if (bip32_path == NULL || address_to_check == NULL) {
        return false;
    }
    unsigned char compressed_public_key[33];
    if (address_to_check_len > MAX_ADDRESS_LENGTH_STR) {
        return false;
    }
    if (address_to_check_len < 1) {
        return false;
    }
    if (!crypto_get_compressed_pubkey_at_path(bip32_path,
                                              bip32_path_len,
                                              compressed_public_key,
                                              NULL)) {
        return false;
    }
    char address_recovered[MAX_ADDRESS_LENGTH_STR + 1];
    if (!get_address_from_compressed_public_key(address_type,
                                                compressed_public_key,
                                                COIN_P2PKH_VERSION,
                                                COIN_P2SH_VERSION,
                                                COIN_NATIVE_SEGWIT_PREFIX,
                                                address_recovered,
                                                sizeof(address_recovered))) {
        PRINTF("Can't create address from given public key\n");
        return false;
    }
    if (os_strcmp(address_recovered, address_to_check) != 0) {
        PRINTF("Addresses don't match\n");
        return false;
    }
    PRINTF("Addresses match\n");
    return true;
}

/**
 * @brief Displays data content and confirms the withdrawal operation.
 *
 * This function retrieves and formats data chunks from a Merkle tree, validates
 * the data, and displays it for user confirmation. It handles the parsing of
 * the data chunks, formatting of the value, and validation of the redeemer address.
 *
 * @param dc Pointer to the dispatcher context.
 * @param data_merkle_root Pointer to the Merkle root of the data.
 * @param n_chunks Number of chunks in the Merkle tree.
 * @param bip32_path Pointer to the BIP32 path.
 * @param bip32_path_len Length of the BIP32 path.
 *
 * @return true if the data is successfully displayed and confirmed, false otherwise.
 */
static bool display_data_content_and_confirm(dispatcher_context_t* dc,
                                             uint8_t* data_merkle_root,
                                             size_t n_chunks,
                                             uint32_t* bip32_path,
                                             uint8_t bip32_path_len) {
    if (dc == NULL || data_merkle_root == NULL || bip32_path == NULL) {
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        return false;
    }
    reset_streaming_index();
    uint8_t data_chunk[CHUNK_SIZE_IN_BYTES];
    char value[AMOUNT_SIZE_IN_CHARS + 1];
    memset(value, 0, sizeof(value));

    // Get the first chunk that contains the data to display
    call_get_merkle_leaf_element(dc,
                                 data_merkle_root,
                                 n_chunks,
                                 DATA_CHUNK_INDEX_1,
                                 data_chunk,
                                 CHUNK_SIZE_IN_BYTES);
    // Start Parsing

    // format value
    int offset_value = CHUNK_SECOND_PART + 24;
    uint64_t value_u64 = read_u64_be(data_chunk, offset_value);

    if (!format_fpu64(value, sizeof(value), value_u64, 18)) {
        return false;
    };

    // Concat the COIN_COINID_SHORT to the value
    // AMOUNT_SIZE_IN_CHARS + ' ' + MAX_TICKER_LEN + '\0'
    char value_with_ticker[AMOUNT_SIZE_IN_CHARS + 1 + MAX_TICKER_LEN + 1];
    snprintf(value_with_ticker, sizeof(value_with_ticker), "stBTC %s", value);

    // Trim the value of trailing zeros in a char of size of value
    int value_with_ticker_len = sizeof(value_with_ticker) - 1;
    int i = value_with_ticker_len;
    while (value_with_ticker[i] == '0' || value_with_ticker[i] == '\0' ||
           value_with_ticker[i] == '.') {
        if (i == 0) {
            break;
        }
        i--;
    }
    if (i < value_with_ticker_len) {
        value_with_ticker[i + 1] = '\0';
    }
    // Get the second chunk that contains the data to display
    call_get_merkle_leaf_element(dc,
                                 data_merkle_root,
                                 n_chunks,
                                 DATA_CHUNK_INDEX_2,
                                 data_chunk,
                                 CHUNK_SIZE_IN_BYTES);
    // get the length from the first 32 bytes of data_chunk. It is the last 2 bytes
    int offset_length = 30;
    size_t len_redeemer_output_script = read_u16_be(data_chunk, offset_length);
    if (len_redeemer_output_script > 32) {
        len_redeemer_output_script = 32;
    }
    const int offset_output_script = CHUNK_SECOND_PART + 1;  // the first byte is the length
    char redeemer_address[MAX_ADDRESS_LENGTH_STR + 1];
    memset(redeemer_address, 0, sizeof(redeemer_address));

    int address_type =
        get_script_type(&data_chunk[offset_output_script],
                        len_redeemer_output_script - 1);  // the first byte is the length

    int redeemer_address_len =
        get_script_address(&data_chunk[offset_output_script],
                           len_redeemer_output_script - 1,  // the first byte is the length
                           (char*) redeemer_address,
                           MAX_ADDRESS_LENGTH_STR);

    if (address_type == -1 || redeemer_address_len == -1) {
        PRINTF("Error: Address type or address length is invalid\n");
        SEND_SW(dc, SW_INCORRECT_DATA);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return false;
    }
    if (!check_address(bip32_path,
                       bip32_path_len,
                       redeemer_address,
                       redeemer_address_len,
                       address_type)) {
        SEND_SW(dc, SW_INCORRECT_DATA);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return false;
    }

    // Display data
    if (!ui_validate_withdraw_data_and_confirm(dc, value_with_ticker, redeemer_address)) {
        return false;
    }

    return true;
}

/**
 * @brief Adds leading zeroes to a source buffer and copies it to a destination buffer.
 *
 * This function clears the destination buffer, calculates the offset where the source data
 * should start, and then copies the source data to the destination buffer starting from
 * the calculated offset. The leading part of the destination buffer will be filled with zeroes.
 *
 * @param dest_buffer Pointer to the destination buffer.
 * @param dest_size Size of the destination buffer.
 * @param src_buffer Pointer to the source buffer.
 * @param src_size Size of the source buffer.
 */
void add_leading_zeroes(uint8_t* dest_buffer,
                        size_t dest_size,
                        uint8_t* src_buffer,
                        size_t src_size) {
    if (dest_buffer == NULL || src_buffer == NULL) {
        PRINTF("Error: Null buffer\n");
        return;
    }
    if (dest_size < src_size) {
        PRINTF("Error: Destination buffer is too small\n");
        return;
    }
    // Clear the destination buffer
    memset(dest_buffer, 0, dest_size);

    // Calculate the offset where the data should start
    size_t buffer_offset = dest_size - src_size;

    // Copy the source data to the destination buffer starting from the calculated offset
    memcpy(dest_buffer + buffer_offset, src_buffer, src_size);
}

/**
 * @brief Fetches a chunk of data from a Merkle tree, processes it, and adds it to a hash context.
 *
 * This function retrieves a specific chunk of data from a Merkle tree using the provided dispatcher
 * context and Merkle root. The chunk is then added to the provided hash context.
 *
 * @param dc                Pointer to the dispatcher context.
 * @param data_merkle_root  Pointer to the Merkle root of the data.
 * @param n_chunks          Total number of chunks in the Merkle tree.
 * @param hash_context      Pointer to the SHA-3 hash context.
 * @param chunk_index       Index of the chunk to fetch.
 * @param chunk_offset      Offset within the chunk to start processing.
 * @param chunk_data_size   Size of the data within the chunk to process.
 */
void fetch_and_add_chunk_to_hash(dispatcher_context_t* dc,
                                 uint8_t* data_merkle_root,
                                 size_t n_chunks,
                                 cx_sha3_t* hash_context,
                                 size_t chunk_index,
                                 size_t chunk_offset,
                                 size_t chunk_data_size) {
    if (dc == NULL || data_merkle_root == NULL || hash_context == NULL) {
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        return;
    }
    uint8_t data_chunk[CHUNK_SIZE_IN_BYTES];
    int current_chunk_len = call_get_merkle_leaf_element(dc,
                                                         data_merkle_root,
                                                         n_chunks,
                                                         chunk_index,
                                                         data_chunk,
                                                         CHUNK_SIZE_IN_BYTES);
    if (current_chunk_len < 0) {
        SAFE_SEND_SW(dc, SW_WRONG_DATA_LENGTH);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }
    size_t field_buffer_size = FIELD_SIZE;
    uint8_t field_buffer[FIELD_SIZE];
    field_buffer_size = chunk_data_size;
    memcpy(field_buffer, data_chunk + chunk_offset, field_buffer_size);

    CX_THROW(cx_hash_no_throw((cx_hash_t*) hash_context,
                              0,                  // mode
                              field_buffer,       // input data
                              field_buffer_size,  // input length
                              NULL,               // output (intermediate)
                              0));                // no output yet
}

/**
 * @brief Fetches a chunk of data from a Merkle tree and adds it to the output buffer.
 *
 * This function retrieves a specific chunk of data from a Merkle tree using the provided
 * dispatcher context and Merkle root. The chunk is then ABI-encoded and added
 * to the specified position in the output buffer.
 *
 * @param dc The dispatcher context used for the operation.
 * @param data_merkle_root The Merkle root of the data tree.
 * @param n_chunks The total number of chunks in the data tree.
 * @param chunk_index The index of the chunk to fetch.
 * @param chunk_offset The offset within the chunk to start reading data from.
 * @param chunk_data_size The size of the data to read from the chunk.
 * @param output_buffer The buffer to which the fetched data will be added.
 * @param output_buffer_offset The offset within the output buffer to start writing data to.
 */
void fetch_and_add_chunk_to_buffer(dispatcher_context_t* dc,
                                   uint8_t* data_merkle_root,
                                   size_t n_chunks,
                                   size_t chunk_index,
                                   size_t chunk_offset,
                                   size_t chunk_data_size,
                                   uint8_t* output_buffer,
                                   size_t output_buffer_offset) {
    if (dc == NULL || data_merkle_root == NULL || output_buffer == NULL) {
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        return;
    }
    uint8_t data_chunk[CHUNK_SIZE_IN_BYTES];
    int current_chunk_len = call_get_merkle_leaf_element(dc,
                                                         data_merkle_root,
                                                         n_chunks,
                                                         chunk_index,
                                                         data_chunk,
                                                         CHUNK_SIZE_IN_BYTES);
    if (current_chunk_len < 0) {
        SAFE_SEND_SW(dc, SW_WRONG_DATA_LENGTH);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }
    size_t input_buffer_size;
    uint8_t input_buffer[32];

    // Abi-encode the data if it is less than 32 bytes
    if (chunk_data_size < 32) {
        input_buffer_size = 32;
        add_leading_zeroes(input_buffer,
                           sizeof(input_buffer),
                           data_chunk + chunk_offset,
                           chunk_data_size);
    } else {
        input_buffer_size = chunk_data_size;
        memcpy(input_buffer, data_chunk + chunk_offset, input_buffer_size);
    }
    memcpy(output_buffer + output_buffer_offset, input_buffer, input_buffer_size);
}

/**
 * @brief Fetches transaction data chunks, hashes them, and stores the result.
 *
 * This function fetches specific chunks of transaction data, adds them to a hash context,
 * and finalizes the hash, storing the result in the provided output buffer.
 *
 * @param[in] dc                Pointer to the dispatcher context.
 * @param[in] data_merkle_root  Pointer to the data Merkle root.
 * @param[in] n_chunks          Number of chunks in the transaction data.
 * @param[in] hash_context      Pointer to the SHA-3 hash context.
 * @param[out] output_buffer    Buffer to store the resulting hash (32 bytes).
 *
 * @note The function fetches and hashes the first 4 bytes of the transaction data separately.
 *       It then fetches and hashes the remaining chunks in 32-byte segments.
 *       The hash is finalized and stored in the output buffer.
 */
void fetch_and_hash_tx_data(dispatcher_context_t* dc,
                            uint8_t* data_merkle_root,
                            size_t n_chunks,
                            cx_sha3_t* hash_context,
                            uint8_t* output_buffer) {
    // Fetch and add the first 4 bytes of the tx.data to the hash
    fetch_and_add_chunk_to_hash(dc, data_merkle_root, n_chunks, hash_context, 4, 0, 4);
    // Fetch and add the other value is tx.data to the hash
    for (size_t i = 5; i < n_chunks; i++) {
        // Fetch and add data[32] to the hash
        fetch_and_add_chunk_to_hash(dc, data_merkle_root, n_chunks, hash_context, i, 0, 32);
        // Fetch and add data[32] to the hash
        fetch_and_add_chunk_to_hash(dc, data_merkle_root, n_chunks, hash_context, i, 32, 32);
    }
    // Finalize the hash and store the result in output_hash
    CX_THROW(cx_hash_no_throw((cx_hash_t*) hash_context,
                              CX_LAST,                 // final block mode
                              NULL,                    // no more input
                              0,                       // no more input length
                              output_buffer,           // output hash buffer
                              KECCAK_256_HASH_SIZE));  // output hash length (32 bytes)
}

/**
 * @brief Fetches and ABI encodes transaction fields into the output buffer.
 *
 * This function retrieves various fields from a transaction, ABI encodes them,
 * and stores them sequentially in the provided output buffer. The fields are
 * fetched from a Merkle tree using the provided dispatcher context and data
 * Merkle root.
 *
 * @param dc Pointer to the dispatcher context used for fetching data.
 * @param data_merkle_root Pointer to the root of the Merkle tree containing the transaction data.
 * @param n_chunks Number of chunks in the Merkle tree.
 * @param keccak_of_tx_data Pointer to the Keccak hash of the transaction data.
 * @param output_buffer Pointer to the buffer where the encoded transaction fields will be stored.
 * @param output_buffer_size Size of the output buffer (must be at least FIELD_SIZE * 11 bytes).
 *
 * @return true if successful, false if buffer size is insufficient
 */
bool fetch_and_abi_encode_tx_fields(dispatcher_context_t* dc,
                                    uint8_t* data_merkle_root,
                                    size_t n_chunks,
                                    uint8_t* keccak_of_tx_data,
                                    uint8_t* output_buffer,
                                    size_t output_buffer_size) {
    if (dc == NULL || data_merkle_root == NULL || output_buffer == NULL) {
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        return false;
    }

    // Check if output buffer is large enough
    const size_t required_size = FIELD_SIZE * 11;
    if (output_buffer_size < required_size) {
        SAFE_SEND_SW(dc, SW_WRONG_DATA_LENGTH);
        return false;
    }

    size_t offset = 0;

    // Copy 'SafeTxTypeHash' field into output_buffer
    memcpy(output_buffer + offset, safe_tx_typehash, 32);
    offset += FIELD_SIZE;
    // Fetch 'to' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc,
                                  data_merkle_root,
                                  n_chunks,
                                  0,
                                  0,
                                  ADDRESS_SIZE_IN_BYTES,
                                  output_buffer,
                                  offset);
    offset += FIELD_SIZE;
    // Fetch 'value' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 1, 0, 32, output_buffer, offset);
    offset += FIELD_SIZE;
    // Add keccak_of_tx_data to output_buffer
    memcpy(output_buffer + offset, keccak_of_tx_data, 32);
    offset += FIELD_SIZE;
    // Fetch 'operation' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 3, 0, 1, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch 'safeTXGas' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 1, 32, 32, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch 'baseGas' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 2, 1, 32, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch 'gasPrice' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 2, 32, 32, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch 'gasToken' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 0, 20, 20, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch 'refundReceiver' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 0, 40, 20, output_buffer, offset);
    offset += FIELD_SIZE;
    // Fetch '_nonce' field, add leading zeroes and add to output_buffer
    fetch_and_add_chunk_to_buffer(dc, data_merkle_root, n_chunks, 3, 0, 32, output_buffer, offset);

    return true;
}

/**
 * @brief Computes the domain separator hash according to EIP-712 specification.
 *
 * This function computes the domain separator hash by combining and hashing:
 * 1. The ABI-encoded EIP-712 domain separator typehash
 * 2. The ABI-encoded chain ID
 * 3. The ABI-encoded verifying contract address
 *
 * The function follows the EIP-712 specification for structured data hashing
 * and signing. The computed hash is used as part of the transaction signing
 * process.
 *
 * @param dc Pointer to the dispatcher context used for operations
 * @param data_merkle_root Pointer to the Merkle root of the transaction data
 * @param n_chunks Number of chunks in the Merkle tree
 * @param output_buffer Buffer to store the computed domain separator hash (32 bytes)
 */
void compute_domain_separator_hash(dispatcher_context_t* dc,
                                   uint8_t* data_merkle_root,
                                   size_t n_chunks,
                                   uint8_t* output_buffer) {
    cx_sha3_t hash_context;
    CX_THROW(cx_keccak_init_no_throw(&hash_context, 256));
    // Add the EIP712 domain separator typehash to the hash context (it is already abi-encoded)
    CX_THROW(cx_hash_no_throw((cx_hash_t*) &hash_context,
                              0,
                              domain_separator_typehash,
                              sizeof(domain_separator_typehash),
                              NULL,
                              0));

    // add the abi encoded chainId to the hash context
    CX_THROW(cx_hash_no_throw((cx_hash_t*) &hash_context,
                              0,
                              abi_encoded_chain_id,
                              sizeof(abi_encoded_chain_id),
                              NULL,
                              0));
    // Add the verifying contract address to the hash context (it is already abi-encoded)
    fetch_and_add_chunk_to_hash(dc, data_merkle_root, n_chunks, &hash_context, 7, 0, 32);
    // Compute the final hash
    CX_THROW(cx_hash_no_throw((cx_hash_t*) &hash_context,
                              CX_LAST,
                              NULL,
                              0,
                              output_buffer,
                              KECCAK_256_HASH_SIZE));
}

/**
 * @brief Computes the transaction hash using Keccak-256.
 *
 * This function performs the following steps:
 * 1. Initializes a SHA-3 context for Keccak-256 (256-bit output).
 * 2. Computes the Keccak-256 hash of the transaction data.
 * 3. Fetches and ABI-encodes the transaction fields.
 * 4. Computes the Keccak-256 hash of the ABI-encoded transaction fields.
 * 5. ABI-encodes the packed data, which includes two Keccak-256 hashes.
 * 6. Computes the Keccak-256 hash of the ABI-encoded packed data.
 *
 * @param dc Pointer to the dispatcher context.
 * @param data_merkle_root Pointer to the data Merkle root.
 * @param n_chunks Number of chunks in the transaction data.
 * @param output_buffer Buffer to store the final computed hash (32 bytes).
 */
void compute_tx_hash(dispatcher_context_t* dc,
                     uint8_t* data_merkle_root,
                     size_t n_chunks,
                     u_int8_t output_buffer[KECCAK_256_HASH_SIZE]) {
    cx_sha3_t hash_context;

    // Initialize the SHA-3 context for Keccak-256 (256-bit output)
    CX_THROW(cx_keccak_init_no_throw(&hash_context, 256));
    u_int8_t keccak_of_tx_data[KECCAK_256_HASH_SIZE];
    // Compute keccak256 hash of the tx_data_data
    fetch_and_hash_tx_data(dc, data_merkle_root, n_chunks, &hash_context, keccak_of_tx_data);

    // Fetch and ABI-encode the tx fields
    u_int8_t abi_encoded_tx_fields[FIELD_SIZE * 11];
    if (!fetch_and_abi_encode_tx_fields(dc,
                                        data_merkle_root,
                                        n_chunks,
                                        keccak_of_tx_data,
                                        abi_encoded_tx_fields,
                                        sizeof(abi_encoded_tx_fields))) {
        return;  // Error already handled in the function
    }

    // Hash the abi_encoded_tx_fields
    u_int8_t keccak_of_abi_encoded_tx_fields[KECCAK_256_HASH_SIZE];
    CX_THROW(cx_keccak_init_no_throw(&hash_context, 256));
    CX_THROW(cx_hash_no_throw((cx_hash_t*) &hash_context,
                              CX_LAST,
                              abi_encoded_tx_fields,
                              sizeof(abi_encoded_tx_fields),
                              keccak_of_abi_encoded_tx_fields,
                              sizeof(keccak_of_abi_encoded_tx_fields)));
    // Compute domain_separator_hash
    uint8_t domain_separator_hash[KECCAK_256_HASH_SIZE];
    compute_domain_separator_hash(dc, data_merkle_root, n_chunks, domain_separator_hash);
    // Abi.encodePacked
    // 2 bytes (0x1901) + 2 keccak256 hashes
    u_int8_t abi_encode_packed[2 + (KECCAK_256_HASH_SIZE * 2)] = {0x19, 0x01};
    // Add the domain_separator_hash to the abi_encode_packed
    memcpy(abi_encode_packed + 2, domain_separator_hash, KECCAK_256_HASH_SIZE);
    // Add the keccak_of_tx_data to the abi_encode_packed
    memcpy(abi_encode_packed + 2 + KECCAK_256_HASH_SIZE,
           keccak_of_abi_encoded_tx_fields,
           KECCAK_256_HASH_SIZE);

    // Keccak256 hash of abi.encodePacked
    // reset the hash context and compute the hash
    CX_THROW(cx_keccak_init_no_throw(&hash_context, 256));
    CX_THROW(cx_hash_no_throw((cx_hash_t*) &hash_context,
                              CX_LAST,
                              abi_encode_packed,
                              sizeof(abi_encode_packed),
                              output_buffer,
                              KECCAK_256_HASH_SIZE));
}

/**
 * @brief Signs a transaction hash using ECDSA with a given BIP32 path.
 *
 * This function computes the Bitcoin Message Signing (BSM) digest of the given transaction hash,
 * then signs the digest using the ECDSA algorithm with the provided BIP32 path.
 *
 * @param[in] dc Dispatcher context.
 * @param[in] bip32_path Pointer to the BIP32 path array.
 * @param[in] bip32_path_len Length of the BIP32 path array.
 * @param[in] tx_hash Pointer to the transaction hash string.
 * @param[out] sig Pointer to the buffer where the signature will be stored.
 *
 * @return The length of the generated signature, or -1 if an error occurred.
 */
uint32_t sign_tx_hash(dispatcher_context_t* dc,
                      uint32_t* bip32_path,
                      uint8_t bip32_path_len,
                      char* tx_hash,
                      uint8_t* sig) {
    size_t tx_hash_length = strlen(tx_hash);
    cx_sha256_t bsm_digest_context;  // used to compute the Bitcoin Message Signing digest
    cx_sha256_init(&bsm_digest_context);

    crypto_hash_update(&bsm_digest_context.header, BSM_SIGN_MAGIC, sizeof(BSM_SIGN_MAGIC));
    crypto_hash_update_varint(&bsm_digest_context.header, tx_hash_length);
    crypto_hash_update(&bsm_digest_context.header, tx_hash, tx_hash_length);

    uint8_t bsm_digest[32];

    crypto_hash_digest(&bsm_digest_context.header, bsm_digest, 32);
    cx_hash_sha256(bsm_digest, 32, bsm_digest, 32);

#ifndef HAVE_AUTOAPPROVE_FOR_PERF_TESTS
    ui_pre_processing_message();
#endif

    uint32_t info;
    int sig_len = crypto_ecdsa_sign_sha256_hash_with_key(bip32_path,
                                                         bip32_path_len,
                                                         bsm_digest,
                                                         NULL,
                                                         sig,
                                                         &info);
    if (sig_len < 0) {
        // unexpected error when signing
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return -1;
    }
    return info;
}

/**
 * @brief Handler for processing withdrawal requests.
 *
 * This file contains the implementation of the handler for processing withdrawal requests.
 * It reads the necessary data from the dispatcher context, validates it, and performs the
 * required cryptographic operations to sign the transaction.
 *
 * @param dc Pointer to the dispatcher context.
 * @param protocol_version The protocol version being used.
 *
 * The function performs the following steps:
 * 1. Reads the BIP32 path length, BIP32 path, number of chunks, and data Merkle root from the
 * dispatcher context's read buffer.
 * 2. Validates the read data and ensures the BIP32 path length does not exceed the maximum allowed
 * steps.
 * 3. Formats the BIP32 path into a string for display.
 * 4. Optionally displays the data content and requests user confirmation (if auto-approve is not
 * enabled).
 * 5. Computes the transaction hash to be signed.
 * 6. Converts the transaction hash to a hexadecimal string for display.
 * 7. Signs the transaction hash using the BIP32 path.
 * 8. Formats the signature into the standard Bitcoin format.
 * 9. Sends the formatted signature as the response.
 * 10. Updates the UI to indicate the result of the operation.
 *
 * If any step fails, the function sends an appropriate status word (SW) and updates the UI to
 * indicate the failure.
 */
void handler_withdraw(dispatcher_context_t* dc, uint8_t protocol_version) {
    (void) protocol_version;

    if (dc == NULL) {
        SAFE_SEND_SW(dc, SW_BAD_STATE);
        return;
    }

    uint8_t bip32_path_len;
    uint32_t bip32_path[MAX_BIP32_PATH_STEPS];
    uint64_t n_chunks;
    uint8_t data_merkle_root[32];

    if (!buffer_read_u8(&dc->read_buffer, &bip32_path_len) ||
        !buffer_read_bip32_path(&dc->read_buffer, bip32_path, bip32_path_len) ||
        !buffer_read_varint(&dc->read_buffer, &n_chunks) ||
        !buffer_read_bytes(&dc->read_buffer, data_merkle_root, 32)) {
        SEND_SW(dc, SW_WRONG_DATA_LENGTH);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }

    if (bip32_path_len > MAX_BIP32_PATH_STEPS) {
        SEND_SW(dc, SW_INCORRECT_DATA);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }

    char path_str[MAX_SERIALIZED_BIP32_PATH_LENGTH + 1] = "(Master key)";
    if (bip32_path_len > 0) {
        bip32_path_format(bip32_path, bip32_path_len, path_str, sizeof(path_str));
    }

#ifndef HAVE_AUTOAPPROVE_FOR_PERF_TESTS
    if (!display_data_content_and_confirm(dc,
                                          data_merkle_root,
                                          n_chunks,
                                          bip32_path,
                                          bip32_path_len)) {
        SEND_SW(dc, SW_DENY);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }

#endif
    // COMPUTE THE HASH THAT WE WILL SIGN
    uint8_t tx_hash[KECCAK_256_HASH_SIZE];
    compute_tx_hash(dc, data_merkle_root, n_chunks, tx_hash);

    // Convert tx_hash to a string for display
    char tx_hash_str[65];
    if (!format_hex(tx_hash, KECCAK_256_HASH_SIZE, tx_hash_str, sizeof(tx_hash_str))) {
        SEND_SW(dc, SW_BAD_STATE);
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    };

    // SIGN MESSAGE (the message is the hash previously computed)
    uint8_t sig[MAX_DER_SIG_LEN];
    uint32_t info = sign_tx_hash(dc, bip32_path, bip32_path_len, tx_hash_str, sig);

    // convert signature to the standard Bitcoin format, always 65 bytes long

    uint8_t result[65];
    memset(result, 0, sizeof(result));

    // # Format signature into standard bitcoin format
    int r_length = sig[3];
    int s_length = sig[4 + r_length + 1];

    if (r_length > 33 || s_length > 33) {
        SEND_SW(dc, SW_BAD_STATE);  // can never happen
        if (!ui_post_processing_confirm_withdraw(dc, false)) {
            PRINTF("Error in ui_post_processing_confirm_withdraw");
        }
        return;
    }

    // Write s, r, and the first byte in reverse order, as the two loops will underflow by 1
    // byte (that needs to be discarded) when s_length and r_length (respectively) are equal
    // to 33.
    for (int i = s_length - 1; i >= 0; --i) {
        result[1 + 32 + 32 - s_length + i] = sig[4 + r_length + 2 + i];
    }
    for (int i = r_length - 1; i >= 0; --i) {
        result[1 + 32 - r_length + i] = sig[4 + i];
    }
    result[0] = 27 + 4 + ((info & CX_ECCINFO_PARITY_ODD) ? 1 : 0);

    SEND_RESPONSE(dc, result, sizeof(result), SW_OK);
    if (!ui_post_processing_confirm_withdraw(dc, true)) {
        PRINTF("Error in ui_post_processing_confirm_withdraw");
    }
    return;
}

/* Copyright (c) 2016-2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file test_hs_service.c
 * \brief Test hidden service functionality.
 */

#define CIRCUITBUILD_PRIVATE
#define CIRCUITLIST_PRIVATE
#define CONNECTION_PRIVATE
#define CRYPTO_PRIVATE
#define HS_COMMON_PRIVATE
#define HS_INTROPOINT_PRIVATE
#define MAIN_PRIVATE
#define TOR_CHANNEL_INTERNAL_

#include "test.h"
#include "log_test_helpers.h"
#include "rend_test_helpers.h"

#include "or.h"
#include "channeltls.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "config.h"
#include "connection.h"
#include "hs_circuit.h"
#include "hs_common.h"
#include "hs_ident.h"
#include "hs_intropoint.h"
#include "hs_ntor.h"
#include "hs_service.h"
#include "main.h"
#include "rendservice.h"

/** We simulate the creation of an outgoing ESTABLISH_INTRO cell, and then we
 *  parse it from the receiver side. */
static void
test_gen_establish_intro_cell(void *arg)
{
  (void) arg;
  ssize_t retval;
  uint8_t circuit_key_material[DIGEST_LEN] = {0};
  uint8_t buf[RELAY_PAYLOAD_SIZE];
  trn_cell_establish_intro_t *cell_out = NULL;
  trn_cell_establish_intro_t *cell_in = NULL;

  crypto_rand((char *) circuit_key_material, sizeof(circuit_key_material));

  /* Create outgoing ESTABLISH_INTRO cell and extract its payload so that we
     attempt to parse it. */
  {
    cell_out = generate_establish_intro_cell(circuit_key_material,
                                             sizeof(circuit_key_material));
    tt_assert(cell_out);

    retval = get_establish_intro_payload(buf, sizeof(buf), cell_out);
    tt_int_op(retval, >=, 0);
  }

  /* Parse it as the receiver */
  {
    ssize_t parse_result = trn_cell_establish_intro_parse(&cell_in,
                                                         buf, sizeof(buf));
    tt_int_op(parse_result, >=, 0);

    retval = verify_establish_intro_cell(cell_in,
                                         circuit_key_material,
                                         sizeof(circuit_key_material));
    tt_int_op(retval, >=, 0);
  }

 done:
  trn_cell_establish_intro_free(cell_out);
  trn_cell_establish_intro_free(cell_in);
}

/* Mocked ed25519_sign_prefixed() function that always fails :) */
static int
mock_ed25519_sign_prefixed(ed25519_signature_t *signature_out,
                           const uint8_t *msg, size_t msg_len,
                           const char *prefix_str,
                           const ed25519_keypair_t *keypair) {
  (void) signature_out;
  (void) msg;
  (void) msg_len;
  (void) prefix_str;
  (void) keypair;
  return -1;
}

/** We simulate a failure to create an ESTABLISH_INTRO cell */
static void
test_gen_establish_intro_cell_bad(void *arg)
{
  (void) arg;
  trn_cell_establish_intro_t *cell = NULL;
  uint8_t circuit_key_material[DIGEST_LEN] = {0};

  MOCK(ed25519_sign_prefixed, mock_ed25519_sign_prefixed);

  crypto_rand((char *) circuit_key_material, sizeof(circuit_key_material));

  setup_full_capture_of_logs(LOG_WARN);
  /* Easiest way to make that function fail is to mock the
     ed25519_sign_prefixed() function and make it fail. */
  cell = generate_establish_intro_cell(circuit_key_material,
                                       sizeof(circuit_key_material));
  expect_log_msg_containing("Unable to gen signature for "
                            "ESTABLISH_INTRO cell.");
  teardown_capture_of_logs();
  tt_assert(!cell);

 done:
  trn_cell_establish_intro_free(cell);
  UNMOCK(ed25519_sign_prefixed);
}

/** Test the HS ntor handshake. Simulate the sending of an encrypted INTRODUCE1
 *  cell, and verify the proper derivation of decryption keys on the other end.
 *  Then simulate the sending of an authenticated RENDEZVOUS1 cell and verify
 *  the proper verification on the other end. */
static void
test_hs_ntor(void *arg)
{
  int retval;

  uint8_t subcredential[DIGEST256_LEN];

  ed25519_keypair_t service_intro_auth_keypair;
  curve25519_keypair_t service_intro_enc_keypair;
  curve25519_keypair_t service_ephemeral_rend_keypair;

  curve25519_keypair_t client_ephemeral_enc_keypair;

  hs_ntor_intro_cell_keys_t client_hs_ntor_intro_cell_keys;
  hs_ntor_intro_cell_keys_t service_hs_ntor_intro_cell_keys;

  hs_ntor_rend_cell_keys_t service_hs_ntor_rend_cell_keys;
  hs_ntor_rend_cell_keys_t client_hs_ntor_rend_cell_keys;

  (void) arg;

  /* Generate fake data for this unittest */
  {
    /* Generate fake subcredential */
    memset(subcredential, 'Z', DIGEST256_LEN);

    /* service */
    curve25519_keypair_generate(&service_intro_enc_keypair, 0);
    ed25519_keypair_generate(&service_intro_auth_keypair, 0);
    curve25519_keypair_generate(&service_ephemeral_rend_keypair, 0);
    /* client */
    curve25519_keypair_generate(&client_ephemeral_enc_keypair, 0);
  }

  /* Client: Simulate the sending of an encrypted INTRODUCE1 cell */
  retval =
    hs_ntor_client_get_introduce1_keys(&service_intro_auth_keypair.pubkey,
                                       &service_intro_enc_keypair.pubkey,
                                       &client_ephemeral_enc_keypair,
                                       subcredential,
                                       &client_hs_ntor_intro_cell_keys);
  tt_int_op(retval, ==, 0);

  /* Service: Simulate the decryption of the received INTRODUCE1 */
  retval =
    hs_ntor_service_get_introduce1_keys(&service_intro_auth_keypair.pubkey,
                                        &service_intro_enc_keypair,
                                        &client_ephemeral_enc_keypair.pubkey,
                                        subcredential,
                                        &service_hs_ntor_intro_cell_keys);
  tt_int_op(retval, ==, 0);

  /* Test that the INTRODUCE1 encryption/mac keys match! */
  tt_mem_op(client_hs_ntor_intro_cell_keys.enc_key, OP_EQ,
            service_hs_ntor_intro_cell_keys.enc_key,
            CIPHER256_KEY_LEN);
  tt_mem_op(client_hs_ntor_intro_cell_keys.mac_key, OP_EQ,
            service_hs_ntor_intro_cell_keys.mac_key,
            DIGEST256_LEN);

  /* Service: Simulate creation of RENDEZVOUS1 key material. */
  retval =
    hs_ntor_service_get_rendezvous1_keys(&service_intro_auth_keypair.pubkey,
                                         &service_intro_enc_keypair,
                                         &service_ephemeral_rend_keypair,
                                         &client_ephemeral_enc_keypair.pubkey,
                                         &service_hs_ntor_rend_cell_keys);
  tt_int_op(retval, ==, 0);

  /* Client: Simulate the verification of a received RENDEZVOUS1 cell */
  retval =
    hs_ntor_client_get_rendezvous1_keys(&service_intro_auth_keypair.pubkey,
                                        &client_ephemeral_enc_keypair,
                                        &service_intro_enc_keypair.pubkey,
                                        &service_ephemeral_rend_keypair.pubkey,
                                        &client_hs_ntor_rend_cell_keys);
  tt_int_op(retval, ==, 0);

  /* Test that the RENDEZVOUS1 key material match! */
  tt_mem_op(client_hs_ntor_rend_cell_keys.rend_cell_auth_mac, OP_EQ,
            service_hs_ntor_rend_cell_keys.rend_cell_auth_mac,
            DIGEST256_LEN);
  tt_mem_op(client_hs_ntor_rend_cell_keys.ntor_key_seed, OP_EQ,
            service_hs_ntor_rend_cell_keys.ntor_key_seed,
            DIGEST256_LEN);

 done:
  ;
}

/** Test that our HS time period calculation functions work properly */
static void
test_time_period(void *arg)
{
  (void) arg;
  uint64_t tn;
  int retval;
  time_t fake_time;

  /* Let's do the example in prop224 section [TIME-PERIODS] */
  retval = parse_rfc1123_time("Wed, 13 Apr 2016 11:00:00 UTC",
                              &fake_time);
  tt_int_op(retval, ==, 0);

  /* Check that the time period number is right */
  tn = get_time_period_num(fake_time);
  tt_u64_op(tn, ==, 16903);

  /* Increase current time to 11:59:59 UTC and check that the time period
     number is still the same */
  fake_time += 3599;
  tn = get_time_period_num(fake_time);
  tt_u64_op(tn, ==, 16903);

  /* Now take time to 12:00:00 UTC and check that the time period rotated */
  fake_time += 1;
  tn = get_time_period_num(fake_time);
  tt_u64_op(tn, ==, 16904);

  /* Now also check our hs_get_next_time_period_num() function */
  tn = hs_get_next_time_period_num(fake_time);
  tt_u64_op(tn, ==, 16905);

 done:
  ;
}

/* Test: Ensure that setting up rendezvous circuits works correctly. */
static void
test_e2e_rend_circuit_setup(void *arg)
{
  ed25519_public_key_t service_pk;
  origin_circuit_t *or_circ;
  int retval;

  /** In this test we create a v3 prop224 service-side rendezvous circuit.
   *  We simulate an HS ntor key exchange with a client, and check that
   *  the circuit was setup correctly and is ready to accept rendezvous data */

  (void) arg;

  /* Now make dummy circuit */
  {
    or_circ = origin_circuit_new();

    or_circ->base_.purpose = CIRCUIT_PURPOSE_S_CONNECT_REND;

    or_circ->build_state = tor_malloc_zero(sizeof(cpath_build_state_t));
    or_circ->build_state->is_internal = 1;

    /* prop224: Setup hs conn identifier on the stream */
    ed25519_secret_key_t sk;
    tt_int_op(0, OP_EQ, ed25519_secret_key_generate(&sk, 0));
    tt_int_op(0, OP_EQ, ed25519_public_key_generate(&service_pk, &sk));

    or_circ->hs_ident = hs_ident_circuit_new(&service_pk,
                                             HS_IDENT_CIRCUIT_RENDEZVOUS);

    TO_CIRCUIT(or_circ)->state = CIRCUIT_STATE_OPEN;
  }

  /* Check number of hops */
  retval = cpath_get_n_hops(&or_circ->cpath);
  tt_int_op(retval, OP_EQ, 0);

  /* Setup the circuit: do the ntor key exchange */
  {
    uint8_t ntor_key_seed[DIGEST256_LEN] = {2};
    retval = hs_circuit_setup_e2e_rend_circ(or_circ,
                                       ntor_key_seed, sizeof(ntor_key_seed),
                                       1);
    tt_int_op(retval, OP_EQ, 0);
  }

  /* See that a hop was added to the circuit's cpath */
  retval = cpath_get_n_hops(&or_circ->cpath);
  tt_int_op(retval, OP_EQ, 1);

  /* Check the digest algo */
  tt_int_op(crypto_digest_get_algorithm(or_circ->cpath->f_digest),
            OP_EQ, DIGEST_SHA3_256);
  tt_int_op(crypto_digest_get_algorithm(or_circ->cpath->b_digest),
            OP_EQ, DIGEST_SHA3_256);
  tt_assert(or_circ->cpath->f_crypto);
  tt_assert(or_circ->cpath->b_crypto);

  /* Ensure that circ purpose was changed */
  tt_int_op(or_circ->base_.purpose, OP_EQ, CIRCUIT_PURPOSE_S_REND_JOINED);

 done:
  circuit_free(TO_CIRCUIT(or_circ));
}

struct testcase_t hs_service_tests[] = {
  { "gen_establish_intro_cell", test_gen_establish_intro_cell, TT_FORK,
    NULL, NULL },
  { "gen_establish_intro_cell_bad", test_gen_establish_intro_cell_bad, TT_FORK,
    NULL, NULL },
  { "hs_ntor", test_hs_ntor, TT_FORK,
    NULL, NULL },
  { "time_period", test_time_period, TT_FORK,
    NULL, NULL },
  { "e2e_rend_circuit_setup", test_e2e_rend_circuit_setup, TT_FORK,
    NULL, NULL },

  END_OF_TESTCASES
};


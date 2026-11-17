/** @file
 * @brief formats group epilogue
 *
 * Nothing to set up or tear down: each test opens its own RPC server
 * and makes its own fixtures, so a test can run alone.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */
#define TE_TEST_NAME    "formats/epilogue"
#include "te_config.h"
#include "tapi_test.h"
int
main(int argc, char **argv)
{
    TEST_START;
    TEST_STEP("formats group epilogue");
    TEST_SUCCESS;
cleanup:
    TEST_END;
}

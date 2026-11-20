/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hal_nfc_priv.h"

pn532_err_t nfc_reader_last_error(void)
{
    return PN532_ERR_NULL;
}

hal_nfc_err_t nfc_reader_err_to_hal(pn532_err_t e)
{
    return HAL_NFC_ERR_COMM;
}

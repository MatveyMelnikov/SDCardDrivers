/*
Initializing SD cards of different versions and sizes
*/

#include "sd_driver_init.h"
#include "math.h"

// Variables -----------------------------------------------------------------

// Displaying the status of the SD card: its version and size
sd_status sd_card_status = { 0 };

// Static functions ----------------------------------------------------------

// To IDLE state
static sd_error sd_card_power_on(SPI_HandleTypeDef *const hspi)
{
  sd_error status = SD_OK;
  uint8_t dummy_data = 0xff;

  DISELECT_SD();

  // Clock occurs only during transmission
  for (uint8_t i = 0; i < 10; i++) // Need at least 74 ticks
    status |= HAL_SPI_Transmit(hspi, &dummy_data, 1, SD_TRANSMISSION_TIMEOUT);

  // Just in case, we get the result
  status |= sd_card_receive_byte(hspi, &dummy_data);
  return status;
}

static sd_error sd_card_enter_spi_mode(SPI_HandleTypeDef *const hspi)
{
  static bool sd_card_is_spi_mode = false;
  sd_command cmd_go_idle_state = sd_card_get_cmd(0, 0x0);
  sd_r1_response r1 = 0;

  if (sd_card_is_spi_mode)
    return SD_OK;

  sd_error status = sd_card_power_on(hspi);
  if (status)
    return status;

  SELECT_SD();
  status |= HAL_SPI_Transmit(
    hspi,
    (uint8_t*)&cmd_go_idle_state,
    sizeof(cmd_go_idle_state),
    SD_TRANSMISSION_TIMEOUT
  );
  status |= sd_card_receive_byte(hspi, &r1);

  uint32_t tickstart = HAL_GetTick();
  do
  {
    status |= sd_card_receive_byte(hspi, &r1);
    if (HAL_GetTick() - tickstart > 500)
      return SD_TIMEOUT;
  } while (r1 != R1_IN_IDLE_STATE);

  DISELECT_SD();

  if (status == SD_OK)
    sd_card_is_spi_mode = true;

  return status;
}

// Host should enable CRC verification before issuing ACMD41
static sd_error sd_card_crc_on_off(
  SPI_HandleTypeDef *const hspi,
  const bool crc_enable
) 
{
  sd_command cmd_crc_on_off = sd_card_get_cmd(59, crc_enable ? 0x1 : 0x0);
  sd_r1_response r1 = { 0 };
  sd_error status = SD_OK;

  SEND_CMD(hspi, cmd_crc_on_off, r1, status);
  if (r1 != R1_IN_IDLE_STATE)
    return SD_TRANSMISSION_ERROR;

  return status;
}

static sd_error sd_card_v1_init_process(SPI_HandleTypeDef *const hspi)
{
  // Reads OCR to get supported voltage
  sd_command cmd_read_ocr = sd_card_get_cmd(58, 0x0);
  // Next - application specific command
  sd_command cmd_app = sd_card_get_cmd(55, 0x0);
  // Send operating conditions
  sd_command acmd_send_op_cond = sd_card_get_cmd(41, 0x0);
  sd_r3_response ocr_response = { 0 };
  sd_r1_response app_response = { 0 };
  sd_r1_response send_op_cond_response = { 0 };
  sd_error status = SD_OK;

  SEND_CMD(hspi, cmd_read_ocr, ocr_response, status);
	
  // Check voltage. The card must support 2.7-3.6V
  // MSB. Second byte is 23 - 16 bits of OCR
  // Third byte starts with 15 bit oof OCR
  if (!((ocr_response.ocr_register_content[1] & 0x1f) &&
    (ocr_response.ocr_register_content[2] & 0x80)))
    return SD_ERROR;

  uint32_t tickstart = HAL_GetTick();
  while (true)
  {
    SEND_CMD(hspi, cmd_app, app_response, status);
    SEND_CMD(hspi, acmd_send_op_cond, send_op_cond_response, status);
		
    if (send_op_cond_response == R1_CLEAR_FLAGS)
      break;
    if (send_op_cond_response & R1_ILLEGAL_COMMAND)
      return SD_UNUSABLE_CARD;
    // Card initialization shall be completed within 1 second 
    // from the first ACMD41
    if (HAL_GetTick() - tickstart > 1000)
      return SD_TIMEOUT;
  }

  sd_card_status.version = 1;
  sd_card_status.capacity = STANDART;

  return status;
}

static sd_error sd_card_v2_init_process(
  SPI_HandleTypeDef *const hspi
)
{
  // Reads OCR to get supported voltage
  sd_command cmd_read_ocr = sd_card_get_cmd(58, 0x0);
   // Next - application specific command
  sd_command cmd_app = sd_card_get_cmd(55, 0x0);
  sd_command acmd_send_op_cond = sd_card_get_cmd(41, 0x0);
  sd_r3_response ocr_response = { 0 };
  sd_r1_response app_response = { 0 };
  sd_r1_response send_op_cond_response = { 0 };
  sd_error status = SD_OK;

  SEND_CMD(hspi, cmd_read_ocr, ocr_response, status);
	
  // Check voltage. The card must support 2.7-3.6V
  // MSB. Second byte is 23 - 16 bits of OCR
  // Third byte starts with 15 bit oof OCR
  if (!((ocr_response.ocr_register_content[1] & 0x1f) &&
    (ocr_response.ocr_register_content[2] & 0x80)))
    return SD_UNUSABLE_CARD;

  uint32_t tickstart = HAL_GetTick();
  while (true)
	{
    SEND_CMD(hspi, cmd_app, app_response, status);
    SEND_CMD(hspi, acmd_send_op_cond, send_op_cond_response, status);
		
    if (send_op_cond_response == R1_CLEAR_FLAGS)
      break;
    // Card initialization shall be completed within 1 second 
    // from the first ACMD41
    if (HAL_GetTick() - tickstart > 1000)
      return SD_TIMEOUT;
  }

  // Check OCR to identify card capacity
  if (ocr_response.ocr_register_content[0] & 0x2) // Check CCS
    sd_card_status.capacity = HIGH_OR_EXTENDED;
  else
    sd_card_status.capacity = STANDART;
  sd_card_status.version = 2;
	
  return status;
}

// Implementations -----------------------------------------------------------

sd_error sd_card_reset(SPI_HandleTypeDef *const hspi, const bool crc_enable)
{
  // 2.7-3.6V and check pattern
  // Send interface condition
  sd_command cmd_send_if_cond = sd_card_get_cmd(8, (1 << 8) | 0x55);
  sd_r7_response send_if_cond_response = { 0 };

  sd_error status = sd_card_enter_spi_mode(hspi);

  if (status)
    goto end_of_initialization;

  SEND_CMD(hspi, cmd_send_if_cond, send_if_cond_response, status);

  if (status)
    goto end_of_initialization;
	
  status |= sd_card_crc_on_off(hspi, crc_enable);

  // Illegal command hence version 1.0 sd card
  if (send_if_cond_response.high_order_part & R1_ILLEGAL_COMMAND)
  {
    status |= sd_card_v1_init_process(hspi);
    goto end_of_initialization;
  }

  // Check pattern or voltage inconsistency
  if (!(send_if_cond_response.echo_back_of_check_pattern == 0x55 &&
    GET_VOLTAGE_FROM_R7(send_if_cond_response) == 0x1))
  {
    status |= SD_UNUSABLE_CARD;
    goto end_of_initialization;
  }
	
  status |= sd_card_v2_init_process(hspi);

end_of_initialization:
  sd_card_status.error_in_initialization = (bool)status;
  return status;
}

sd_error sd_card_get_common_info(
  SPI_HandleTypeDef *const hspi, sd_info *const info
)
{
  uint8_t csd_register[16] = { 0 };

  // We request the CSD register to check the ability to set the block size
  sd_error status = sd_card_get_csd(hspi, (uint8_t *const)&csd_register);
  float transfer_rate_unit[] = { 0.1f, 1.f, 10.f, 100.f };
  float time_value[] = { 
    0.f, 1.f, 1.2f, 1.3f, 1.5f, 2.f, 
    2.5f, 3.f, 3.5f, 4.f, 4.5f, 
    5.f, 5.5f, 6.f, 7.f, 8.f 
  };

  if (status)
    return status;

  info->max_transfer_speed = transfer_rate_unit[csd_register[3] & 0x7] *
    time_value[(csd_register[3] & 0x78) >> 3];
  info->command_classes = (csd_register[4] << 4) | 
    ((csd_register[5] & 0xf0) >> 4);
  info->max_data_block_size = powf(2.f, (float)(csd_register[5] & 0xf));
  info->partial_blocks_allowed = csd_register[6] & 0x80;

  // Calculate size
  if (sd_card_status.version == 1)
  {
    uint8_t size_mult = (csd_register[9] & 0xe0) >> 5;
    uint32_t mult = (uint32_t)(powf(2.f, (float)size_mult + 2) + 0.5f);
    uint16_t device_size = ((uint32_t)(csd_register[6] & 0x3) << 11) |
      ((uint32_t)csd_register[7] << 2) | 
      ((uint32_t)(csd_register[8] & 0xc0) >> 6);
    uint32_t blocknr = (device_size + 1) * mult;
    info->size = blocknr * info->max_data_block_size;
  }
  else
  {
    uint32_t c_size = ((uint32_t)(csd_register[7] & 0x3f) << 16) | 
      ((uint32_t)(csd_register[8]) << 8) | csd_register[9];
    info->size = (c_size + 1) * 512;
  }

  return SD_OK;
}

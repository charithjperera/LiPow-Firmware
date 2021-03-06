/**
 ******************************************************************************
 * @file           : bq25703a_regulator.c
 * @brief          : Handles battery state information
 ******************************************************************************
 */

#include "adc_interface.h"
#include "bq25703a_regulator.h"
#include "battery.h"
#include "error.h"
#include "main.h"
#include "string.h"
#include "printf.h"
#include "usbpd.h"

extern I2C_HandleTypeDef hi2c1;

/* Private typedef -----------------------------------------------------------*/
struct Regulator {
	uint8_t connected;
	uint8_t charging_status;
	uint16_t max_charge_voltage;
	uint8_t input_current_limit;
	uint16_t min_input_voltage_limit;
	uint32_t vbus_voltage;
	uint32_t vbat_voltage;
	uint32_t vsys_voltage;
	uint32_t charge_current;
	uint32_t input_current;
	uint32_t max_charge_current_ma;
};

/* Private variables ---------------------------------------------------------*/
struct Regulator regulator;
uint8_t precharging_state=0;

/* The maximum time to wait for the mutex that guards the UART to become
 available. */
#define cmdMAX_MUTEX_WAIT	pdMS_TO_TICKS( 300 )

/* Private function prototypes -----------------------------------------------*/
void I2C_Transfer(uint8_t *pData, uint16_t size);
void I2C_Receive(uint8_t *pData, uint16_t size);
void I2C_Write_Register(uint8_t addr_to_write, uint8_t *pData);
void I2C_Write_Two_Byte_Register(uint8_t addr_to_write, uint8_t lsb_data, uint8_t msb_data);
void I2C_Read_Register(uint8_t addr_to_read, uint8_t *pData, uint16_t size);
uint8_t Query_Regulator_Connection(void);
uint8_t Read_Charge_Okay(void);
void Read_Charge_Status(void);
void Regulator_Set_ADC_Option(void);
void Regulator_Read_ADC(void);
void Regulator_HI_Z(uint8_t hi_z_en);
void Regulator_OTG_EN(uint8_t otg_en);
void Regulator_Set_Charge_Option_0(void);
void Set_Charge_Voltage(uint8_t number_of_cells);

/**
 * @brief Returns whether the regulator is connected over I2C
 * @retval uint8_t CONNECTED or NOT_CONNECTED
 */
uint8_t Get_Regulator_Connection_State() {
	return regulator.connected;
}

/**
 * @brief Returns whether the regulator is charging
 * @retval uint8_t 1 if charging, 0 if not charging
 */
uint8_t Get_Regulator_Charging_State() {
	return regulator.charging_status;
}

/**
 * @brief Gets VBAT voltage that was read in from the ADC on the regulator
 * @retval VBAT voltage in volts * REG_ADC_MULTIPLIER
 */
uint32_t Get_VBAT_ADC_Reading() {
	return regulator.vbat_voltage;
}

/**
 * @brief Gets VBUS voltage that was read in from the ADC on the regulator
 * @retval VBUS voltage in volts * REG_ADC_MULTIPLIER
 */
uint32_t Get_VBUS_ADC_Reading() {
	return regulator.vbus_voltage;
}

/**
 * @brief Gets Input Current that was read in from the ADC on the regulator
 * @retval Input Current in amps * REG_ADC_MULTIPLIER
 */
uint32_t Get_Input_Current_ADC_Reading() {
	return regulator.input_current;
}

/**
 * @brief Gets Charge Current that was read in from the ADC on the regulator
 * @retval Charge Current in amps * REG_ADC_MULTIPLIER
 */
uint32_t Get_Charge_Current_ADC_Reading() {
	return regulator.charge_current;
}

/**
 * @brief Gets the max output current for charging
 * @retval Max Charge Current in miliamps
 */
uint32_t Get_Max_Charge_Current() {
	return regulator.max_charge_current_ma;
}

/**
 * @brief Returns whether we are in the precharge state or not
 * @retval uint8_t 1 or 0
 */
uint8_t Get_Precharge_State() {
  return precharging_state;
}

/**
 * @brief Performs an I2C transfer
 * @param pData Pointer to location of data to transfer
 * @param size Size of data to be transferred
 */
void I2C_Transfer(uint8_t *pData, uint16_t size) {

	if ( xSemaphoreTake( xTxMutex_Regulator, cmdMAX_MUTEX_WAIT ) == pdPASS) {
		do
		{
			TickType_t xtimeout_start = xTaskGetTickCount();
			while (HAL_I2C_Master_Transmit_DMA(&hi2c1, (uint16_t)BQ26703A_I2C_ADDRESS, pData, size) != HAL_OK) {
				if (((xTaskGetTickCount()-xtimeout_start)/portTICK_PERIOD_MS) > I2C_TIMEOUT) {
					Set_Error_State(REGULATOR_COMMUNICATION_ERROR);
					break;
				}
			}
		    while (HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) {
				if (((xTaskGetTickCount()-xtimeout_start)/portTICK_PERIOD_MS) > I2C_TIMEOUT) {
					Set_Error_State(REGULATOR_COMMUNICATION_ERROR);
					break;
				}
		    }
		}
		while(HAL_I2C_GetError(&hi2c1) == HAL_I2C_ERROR_AF);
		xSemaphoreGive(xTxMutex_Regulator);
	}
}

/**
 * @brief Performs an I2C transfer
 * @param pData Pointer to location to store received data
 * @param size Size of data to be received
 */
void I2C_Receive(uint8_t *pData, uint16_t size) {
	if ( xSemaphoreTake( xTxMutex_Regulator, cmdMAX_MUTEX_WAIT ) == pdPASS) {
		do
		{
			TickType_t xtimeout_start = xTaskGetTickCount();
			while (HAL_I2C_Master_Receive_DMA(&hi2c1, (uint16_t)BQ26703A_I2C_ADDRESS, pData, size) != HAL_OK) {
				if (((xTaskGetTickCount()-xtimeout_start)/portTICK_PERIOD_MS) > I2C_TIMEOUT) {
					Set_Error_State(REGULATOR_COMMUNICATION_ERROR);
					break;
				}
			}
			while (HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) {
				if (((xTaskGetTickCount()-xtimeout_start)/portTICK_PERIOD_MS) > I2C_TIMEOUT) {
					Set_Error_State(REGULATOR_COMMUNICATION_ERROR);
					break;
				}
			}
		}
		while(HAL_I2C_GetError(&hi2c1) == HAL_I2C_ERROR_AF);
		xSemaphoreGive(xTxMutex_Regulator);
	}
}

/**
 * @brief Automatically performs two I2C writes to write a register on the regulator
 * @param pData Pointer to data to be transferred
 */
void I2C_Write_Register(uint8_t addr_to_write, uint8_t *pData) {
	uint8_t data[2];
	data[0] = addr_to_write;
	data[1] = *pData;
	I2C_Transfer(data, 2);
}

/**
 * @brief Automatically performs three I2C writes to write a two byte register on the regulator
 * @param lsb_data Pointer to least significant byte of data to be transferred
 * @param msb_data Pointer to most significant byte of data to be transferred
 */
void I2C_Write_Two_Byte_Register(uint8_t addr_to_write, uint8_t lsb_data, uint8_t msb_data) {

	uint8_t data[3];
	data[0] = addr_to_write;
	data[1] = lsb_data;
	data[2] = msb_data;

	I2C_Transfer(data, 3);
}

/**
 * @brief Automatically performs one I2C write and an I2C read to get the value of a register
 * @param pData Pointer to where to store data
 */
void I2C_Read_Register(uint8_t addr_to_read, uint8_t *pData, uint16_t size) {
		I2C_Transfer((uint8_t *)&addr_to_read, 1);
		I2C_Receive(pData, size);
}

/**
 * @brief Checks if the regulator is connected over I2C
 * @retval uint8_t CONNECTED or NOT_CONNECTED
 */
uint8_t Query_Regulator_Connection() {
	/* Get the manufacturer id */
	uint8_t manufacturer_id;
	I2C_Read_Register(MANUFACTURER_ID_ADDR, (uint8_t *) &manufacturer_id, 1);

	/* Get the device id */
	uint8_t device_id;
	I2C_Read_Register(DEVICE_ID_ADDR, (uint8_t *) &device_id, 1);

	if ( (device_id == BQ26703A_DEVICE_ID) && (manufacturer_id == BQ26703A_MANUFACTURER_ID) ) {
		Clear_Error_State(REGULATOR_COMMUNICATION_ERROR);
		return CONNECTED;
	}
	else {
		Set_Error_State(REGULATOR_COMMUNICATION_ERROR);
		return NOT_CONNECTED;
	}
}

/**
 * @brief Checks the state of the Charge okay pin and returns the value
 * @retval 0 if VBUS falls below 3.2 V or rises above 26 V, 1 if VBUS is between 3.5V and 24.5V
 */
uint8_t Read_Charge_Okay() {
	return HAL_GPIO_ReadPin(CHRG_OK_GPIO_Port, CHRG_OK_Pin);
}

/**
 * @brief Reads ChargeStatus register and sets status
 */
void Read_Charge_Status() {
	uint8_t data[2];
	I2C_Read_Register(CHARGE_STATUS_ADDR, data, 2);

	if (data[1] & CHARGING_ENABLED_MASK) {
		regulator.charging_status = 1;
	}
	else {
		regulator.charging_status = 0;
	}
}

/**
 * @brief Sets the Regulators ADC settings
 */
void Regulator_Set_ADC_Option() {

	uint8_t ADC_lsb_3A = ADC_ENABLED_BITMASK;

	I2C_Write_Register(ADC_OPTION_ADDR, (uint8_t *) &ADC_lsb_3A);
}

/**
 * @brief Initiates and reads a single ADC conversion on the regulator
 */
void Regulator_Read_ADC() {
	TickType_t xDelay = 80 / portTICK_PERIOD_MS;

	uint8_t ADC_msb_3B = ADC_START_CONVERSION_MASK;

	I2C_Write_Register((ADC_OPTION_ADDR+1), (uint8_t *) &ADC_msb_3B);

	/* Wait for the conversion to finish */
	while (ADC_msb_3B & (1<<6)) {
		vTaskDelay(xDelay);
		I2C_Read_Register((ADC_OPTION_ADDR+1), (uint8_t *) &ADC_msb_3B, 1);
	}

	uint8_t temp = 0;

	I2C_Read_Register(VBAT_ADC_ADDR, (uint8_t *) &temp, 1);
	regulator.vbat_voltage = (temp * VBAT_ADC_SCALE) + VBAT_ADC_OFFSET;

	I2C_Read_Register(VSYS_ADC_ADDR, (uint8_t *) &temp, 1);
	regulator.vsys_voltage = (temp * VSYS_ADC_SCALE) + VSYS_ADC_OFFSET;

	I2C_Read_Register(ICHG_ADC_ADDR, (uint8_t *) &temp, 1);
	regulator.charge_current = temp * ICHG_ADC_SCALE;

	I2C_Read_Register(IIN_ADC_ADDR, (uint8_t *) &temp, 1);
	regulator.input_current = temp * IIN_ADC_SCALE;

	I2C_Read_Register(VBUS_ADC_ADDR, (uint8_t *) &temp, 1);
	regulator.vbus_voltage = (temp * VBUS_ADC_SCALE) + VBUS_ADC_OFFSET;
}

/**
 * @brief Enables or disables high impedance mode on the output of the regulator
 * @param hi_z_en 1 puts the output of the regulator in hiz mode. 0 takes the regulator out of hi_z and allows charging
 */
void Regulator_HI_Z(uint8_t hi_z_en) {
	if (hi_z_en == 1) {
		HAL_GPIO_WritePin(ILIM_HIZ_GPIO_Port, ILIM_HIZ_Pin, GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOA, FAN_ENn_Pin, GPIO_PIN_SET); // Evil hack to turn the fan off along with the regulator
	}
	else {
		HAL_GPIO_WritePin(ILIM_HIZ_GPIO_Port, ILIM_HIZ_Pin, GPIO_PIN_SET);
		HAL_GPIO_WritePin(GPIOA, FAN_ENn_Pin, GPIO_PIN_RESET); // Evil hack to turn the fan on along with the regulator
	}
}

/**
 * @brief Enables or disables On the Go Mode
 * @param otg_en 0 disables On the GO Mode. 1 Enables.
 */
void Regulator_OTG_EN(uint8_t otg_en) {
	if (otg_en == 0) {
		HAL_GPIO_WritePin(EN_OTG_GPIO_Port, EN_OTG_Pin, GPIO_PIN_RESET);
	}
	else {
		HAL_GPIO_WritePin(EN_OTG_GPIO_Port, EN_OTG_Pin, GPIO_PIN_SET);
	}
}

/**
 * @brief Sets Charge Option 0 Based on #defines in header
 */
void Regulator_Set_Charge_Option_0() {

	uint8_t charge_option_0_register_1_value = 0b00100110;
	uint8_t charge_option_0_register_2_value = 0b00001110;

	I2C_Write_Two_Byte_Register(CHARGE_OPTION_0_ADDR, charge_option_0_register_2_value, charge_option_0_register_1_value);

	return;
}

/**
 * @brief Sets the charging current limit. From 64mA to 8.128A in 64mA steps. Maps from 0 - 128. 7 bit value.
 * @param charge_current_limit Charge current limit in mA
 */
void Set_Charge_Current(uint32_t charge_current_limit) {

	uint32_t charge_current = 0;

	if (charge_current_limit > MAX_CHARGE_CURRENT_MA) {
		charge_current_limit = MAX_CHARGE_CURRENT_MA;
	}

	regulator.max_charge_current_ma = charge_current_limit;

	if (charge_current_limit != 0){
		charge_current = charge_current_limit/64;
	}

	if (charge_current > 128) {
		charge_current = 128;
	}

	//0-128 which remaps from 64mA-8.128A. 7 bit value.
	uint8_t charge_current_register_1_value = 0;
	uint8_t charge_current_register_2_value = 0;

	if ((charge_current >= 0) || (charge_current <= 128)) {
		charge_current_register_1_value = (charge_current >> 2);
		charge_current_register_2_value = (charge_current << 6);
	}

	I2C_Write_Two_Byte_Register(CHARGE_CURRENT_ADDR, charge_current_register_2_value, charge_current_register_1_value);

	return;
}

/**
 * @brief Sets the charging voltage based on the number of cells. 1 - 4.192V, 2 - 8.400V, 3 - 12.592V, 4 - 16.800V
 * @param number_of_cells number of cells connected
 */
void Set_Charge_Voltage(uint8_t number_of_cells) {

	uint8_t max_charge_register_1_value = 0;
	uint8_t max_charge_register_2_value = 0;

	uint8_t	minimum_system_voltage_value = MIN_VOLT_ADD_1024_MV;

#if FIXED_VOLTAGE_CHARGING
	uint16_t target_charge_voltage = FIXED_VOLTAGE_SETPOINT;
	uint16_t fastcharge_threshold = FIXED_VOLTAGE_PRECHARGE;


	max_charge_register_1_value = (target_charge_voltage & 0xFF00) >> 8;
  max_charge_register_2_value = (target_charge_voltage & 0x00FF);
  minimum_system_voltage_value = (fastcharge_threshold & 0xFF00) >> 8;

#else
	if ((number_of_cells > 0) || (number_of_cells < 5)) {
		switch (number_of_cells) {
			case 1:
				max_charge_register_1_value = MAX_VOLT_ADD_4096_MV;
				max_charge_register_2_value = MAX_VOLT_ADD_64_MV | MAX_VOLT_ADD_32_MV;
				minimum_system_voltage_value = MIN_VOLT_ADD_2048_MV | MIN_VOLT_ADD_512_MV | MIN_VOLT_ADD_256_MV;
				break;
			case 2:
				max_charge_register_1_value = MAX_VOLT_ADD_8192_MV;
				max_charge_register_2_value = MAX_VOLT_ADD_128_MV | MAX_VOLT_ADD_64_MV | MAX_VOLT_ADD_16_MV;
				minimum_system_voltage_value = MIN_VOLT_ADD_4096_MV | MIN_VOLT_ADD_1024_MV | MIN_VOLT_ADD_512_MV;
				break;
			case 3:
				max_charge_register_1_value = MAX_VOLT_ADD_8192_MV | MAX_VOLT_ADD_4096_MV | MAX_VOLT_ADD_256_MV;
				max_charge_register_2_value = MAX_VOLT_ADD_32_MV | MAX_VOLT_ADD_16_MV;
				minimum_system_voltage_value = MIN_VOLT_ADD_8192_MV |  MIN_VOLT_ADD_256_MV;
				break;
			case 4:
				max_charge_register_1_value = MAX_VOLT_ADD_16384_MV | MAX_VOLT_ADD_256_MV;
				max_charge_register_2_value = MAX_VOLT_ADD_128_MV | MAX_VOLT_ADD_32_MV;
				minimum_system_voltage_value = MIN_VOLT_ADD_8192_MV | MIN_VOLT_ADD_2048_MV | MIN_VOLT_ADD_1024_MV;
				break;
			default:
				max_charge_register_1_value = 0;
				max_charge_register_2_value = 0;
				minimum_system_voltage_value = MIN_VOLT_ADD_1024_MV;
				break;
			}
	}
#endif

	I2C_Write_Register(MINIMUM_SYSTEM_VOLTAGE_ADDR, (uint8_t *) &minimum_system_voltage_value);

	I2C_Write_Two_Byte_Register(MAX_CHARGE_VOLTAGE_ADDR, max_charge_register_2_value, max_charge_register_1_value);

	return;
}

/**
 * @brief Calculates the max charge power based on temperature of MCU
 * @retval Max charging power in mW
 */
uint32_t Calculate_Max_Charge_Power() {

	//Account for system losses with ASSUME_EFFICIENCY fudge factor to not overload source
	uint32_t charging_power_mw = (((float)(regulator.vbus_voltage/REG_ADC_MULTIPLIER) * Get_Max_Input_Current()) * ASSUME_EFFICIENCY);

	if (charging_power_mw > MAX_CHARGING_POWER) {
		charging_power_mw = MAX_CHARGING_POWER;
	}

	if (charging_power_mw > Get_Max_Input_Power()){
		charging_power_mw = Get_Max_Input_Power() * ASSUME_EFFICIENCY;
	}

	//Throttle charging power if temperature is too high
	if (Get_MCU_Temperature() > TEMP_THROTTLE_THRESH_C){
		float temperature = (float)Get_MCU_Temperature();

		float power_scalar = 1.0f - ((float)(0.0333 * temperature) - 1.66f);

		if (power_scalar > 1.0f) {
			power_scalar = 1.0f;
		}
		if (power_scalar < 0.00f) {
			power_scalar = 0.00f;
		}

		charging_power_mw = charging_power_mw * power_scalar;
	}

	return charging_power_mw;
}


/**
 * @brief Determines if charger output should be on and sets voltage and current parameters as needed
 */
void Control_Charger_Output() {

	TickType_t xDelay = 500 / portTICK_PERIOD_MS;

	static uint16_t termination_counter = 0; // Variable to keep track of termination samples


#if ENABLE_BALANCING
	uint8_t  balance_connection_state = Get_Balance_Connection_State();
#else
	uint8_t  balance_connection_state = CONNECTED;
#endif

	//Charging for USB PD enabled supplies
	if ((Get_XT60_Connection_State() == CONNECTED) && (balance_connection_state == CONNECTED) && (Get_Error_State() == 0) && (Get_Input_Power_Ready() == READY) && (Get_Cell_Over_Voltage_State() == 0)) {

#if ENABLE_BALANCING
		Set_Charge_Voltage(Get_Number_Of_Cells());
#else
		Set_Charge_Voltage(NUM_SERIES);
#endif


		uint32_t charging_current_ma = ((Calculate_Max_Charge_Power()) / (float)(Get_Battery_Voltage() / BATTERY_ADC_MULTIPLIER));

		Set_Charge_Current(charging_current_ma);

		Regulator_HI_Z(0);

		//Check if XT60 was disconnected
		if (regulator.vbat_voltage > (BATTERY_DISCONNECT_THRESH * Get_Number_Of_Cells())) {
			Regulator_HI_Z(1);
			vTaskDelay(xDelay*2);
			Regulator_HI_Z(0);
		}

		float charge_current_meas_ma = ((float)Get_Charge_Current_ADC_Reading()/REG_ADC_MULTIPLIER)*1000;

		if ((Get_Requires_Charging_State() == 0) && (charge_current_meas_ma < CHARGE_TERM_CURRENT_MA)){
		  termination_counter++;
		  if(termination_counter > 3){
		    Regulator_HI_Z(1);
		    vTaskDelay(xDelay);
		  }
		}else{
		  termination_counter = 0;
		}
	}
	// Case to handle non USB PD supplies. Limited to 5V 500mA.
//	else if ((Get_XT60_Connection_State() == CONNECTED) && (balance_connection_state == CONNECTED) && (Get_Error_State() == 0) && (Get_Input_Power_Ready() == NO_USB_PD_SUPPLY) && (Get_Cell_Over_Voltage_State() == 0)) {
//
//#if ENABLE_BALANCING
//    Set_Charge_Voltage(Get_Number_Of_Cells());
//#else
//    Set_Charge_Voltage(NUM_SERIES);
//#endif
//
//		uint32_t charging_current_ma = ((NON_USB_PD_CHARGE_POWER * ASSUME_EFFICIENCY) / (Get_Battery_Voltage() / BATTERY_ADC_MULTIPLIER));
//
//		Set_Charge_Current(charging_current_ma);
//
//		Regulator_HI_Z(0);
//
//	}
	else {
		Regulator_HI_Z(1);
		Set_Charge_Voltage(0);
		Set_Charge_Current(0);
	}
}

/**
 * @brief Main regulator task
 */
void vRegulator(void const *pvParameters) {

	TickType_t xDelay = 250 / portTICK_PERIOD_MS;

	/* Precharge timeout at bootup for 3s * precharge_timeout */
	static uint16_t precharge_timeout = 300; //Up to 3*300 second UVP recovery precharge
	static uint8_t initial_precharge_wakeup = 1; //Apply a longer wakeup pulse to see if that's able to wake up the BQ

	/* Disable the output of the regulator for safety */
	Regulator_HI_Z(1);

	/* Disable OTG mode */
	Regulator_OTG_EN(0);

	/* Check if the regulator is connected */
	regulator.connected = Query_Regulator_Connection();

	/* Set Charge Option 0 */
	Regulator_Set_Charge_Option_0();

	/* Setup the ADC on the Regulator */
	Regulator_Set_ADC_Option();

	vTaskDelay(xDelay); //This was just added to test if init can be made smoother

	for (;;) {

		//Check if power into regulator is okay
		if (Read_Charge_Okay() != 1) {
			Set_Error_State(VOLTAGE_INPUT_ERROR);
		}
		else if ((Get_Error_State() & VOLTAGE_INPUT_ERROR) == VOLTAGE_INPUT_ERROR) {
			Clear_Error_State(VOLTAGE_INPUT_ERROR);
		}

		//Check if STM32G0 can communicate with regulator
		if ((Get_Error_State() & REGULATOR_COMMUNICATION_ERROR) == REGULATOR_COMMUNICATION_ERROR) {
			regulator.connected = 0;
		}

    Read_Charge_Status();

    Regulator_Read_ADC();

#if ATTEMPT_UVP_RECOVERY
		/* Loop through here upon bootup to try recovering a UVP pack */
		float regulator_vbat_voltage = ((float)Get_VBAT_ADC_Reading()/REG_ADC_MULTIPLIER);

		// Precharge until we exceed 12.4V or we hit the timeout. Leave at least one in precharge_timeout as a flag to disable the regulator after this loop
		while((precharge_timeout>1) && (regulator_vbat_voltage < (NUM_SERIES * 3.1))){
		  precharging_state = 1;

      uint8_t ticks = 0;
      if(initial_precharge_wakeup){
        initial_precharge_wakeup = 0;
        ticks = 20;
      }else{
        ticks = 12;
      }

      while(ticks){
        Set_Charge_Voltage(NUM_SERIES);
        Set_Charge_Current(UVP_RECOVERY_CURRENT_MA);
        Regulator_HI_Z(0);
        Read_Charge_Status();
        Regulator_Read_ADC();

        vTaskDelay(xDelay);
        ticks--;
      }

      //Regulator_Read_ADC(); //TBD
      //Read_Charge_Status(); // TBD

		  regulator_vbat_voltage = ((float)Get_VBAT_ADC_Reading()/REG_ADC_MULTIPLIER);
		  precharge_timeout = precharge_timeout - 1;
		}

		if(precharge_timeout){
		  precharging_state = 0;
		  precharge_timeout = 0;
      Regulator_HI_Z(1);

      uint8_t ticks = 4;

      while(ticks){
        vTaskDelay(xDelay);
        Read_Charge_Status();
        Regulator_Read_ADC();
        ticks--;
      }
      //Read_Charge_Status(); // TBD
      //Regulator_Read_ADC(); // TBD

		}
#endif

#if CONTINUOUS_UVP_RECOVERY
		uint16_t zero_volt_tracker = 0;
		if (regulator_vbat_voltage < (NUM_SERIES * 3.1)){
		  zero_volt_tracker++;
		}
#endif



#if ENABLE_BALANCING
		uint8_t timer_count = 0;

		timer_count++;
		if (timer_count < 90) {
			Control_Charger_Output();
		}
		else if (timer_count > 100){
			timer_count = 0;
		}
		else {
			Regulator_HI_Z(1);
		}
#else
		Control_Charger_Output();
#endif

		vTaskDelay(xDelay);
	}
}

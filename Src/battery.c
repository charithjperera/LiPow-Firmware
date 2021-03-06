/**
 ******************************************************************************
 * @file           : battery.c
 * @brief          : Handles battery state information
 ******************************************************************************
 */

#include "adc_interface.h"
#include "battery.h"
#include "bq25703a_regulator.h"
#include "main.h"
#include "printf.h"

/* Private typedef -----------------------------------------------------------*/
struct Battery {
	uint8_t xt60_connected;
	uint8_t balance_port_connected;
	uint8_t number_of_cells;
	uint8_t balancing_enabled;
	uint8_t requires_charging;
	uint8_t cell_over_voltage;
	uint8_t cell_balance_bitmask;
};

/* Private variables ---------------------------------------------------------*/
volatile struct Battery battery_state;

static uint8_t cell_connected_bitmask = 0;

/* Private function prototypes -----------------------------------------------*/
void Balance_Battery(void);
void Balance_Connection_State(void);
void Balancing_GPIO_Control(uint8_t cell_balancing_gpio_bitmask);
void MCU_Temperature_Safety_Check(void);

/**
 * @brief Based on ADC readings, determine if balancing is needed, if so, balance battery
 */
void Balance_Battery()
{
	if ( ENABLE_BALANCING && (battery_state.balance_port_connected == CONNECTED) && (Get_Error_State() == 0) ) {

		uint32_t min_cell_voltage = Get_Cell_Voltage(0);
		uint32_t max_cell_voltage = Get_Cell_Voltage(0);
		for(int i = 1; i < battery_state.number_of_cells; i++) {
			if (Get_Cell_Voltage(i) < min_cell_voltage) {
				min_cell_voltage = Get_Cell_Voltage(i);
			}
			if (Get_Cell_Voltage(i) > max_cell_voltage) {
				max_cell_voltage = Get_Cell_Voltage(i);
			}
		}

		float scalar = 0.0f;

		// Scale the balancing thresholds tighter as the battery voltage increases. Allows for faster charging.
		if (battery_state.xt60_connected == CONNECTED) {
			scalar = (float)CELL_BALANCING_SCALAR_MAX * (1.0f - (((float)max_cell_voltage - (float)MIN_CELL_V_FOR_BALANCING)/((float)CELL_VOLTAGE_TO_ENABLE_CHARGING - (float)MIN_CELL_V_FOR_BALANCING)));
			if (scalar < 1.0f) {
				scalar = 1.0f;
			}
		}
		else {
			scalar = 1.0f;
		}

		if ( ((max_cell_voltage - min_cell_voltage) >= ((float)CELL_DELTA_V_ENABLE_BALANCING * scalar)) && (min_cell_voltage > MIN_CELL_V_FOR_BALANCING) && (battery_state.balancing_enabled == 0)) {
			battery_state.balancing_enabled = 1;
		}
		else if ( (((max_cell_voltage - min_cell_voltage) < ((float)CELL_BALANCING_HYSTERESIS_V * scalar)) && (battery_state.balancing_enabled == 1)) || (min_cell_voltage < MIN_CELL_V_FOR_BALANCING) ) {
			battery_state.balancing_enabled = 0;
		}

		//Check each cell voltage. If XT60 is connected, then allow larger voltage differences that tighten as the battery voltage increases.
		//If just the balance port is connected, then use the tightest balancing thresholds
		//If a cell is over CELL_OVER_VOLTAGE_ENABLE_DISCHARGE, then the discharging resistor will turn on
		for(int i = 0; i < battery_state.number_of_cells; i++) {
			if ( (battery_state.balancing_enabled == 1) && ((Get_Cell_Voltage(i) - min_cell_voltage) >= ((float)CELL_BALANCING_HYSTERESIS_V * scalar))) {
				battery_state.cell_balance_bitmask |= (1<<i);
			}
			else if (Get_Cell_Voltage(i) >= CELL_OVER_VOLTAGE_ENABLE_DISCHARGE) {
				battery_state.cell_balance_bitmask |= (1<<i);
			}
			else {
				battery_state.cell_balance_bitmask &= ~(1<<i);
			}
		}
		Balancing_GPIO_Control(battery_state.cell_balance_bitmask);

	}
	else {
		Balancing_GPIO_Control(0);
		battery_state.balancing_enabled = 0;
	}
}

/**
 * @brief Determines the state of the balance connection based on ADC readings
 */
void Balance_Connection_State()
{
	if (( Get_Four_S_Voltage() > VOLTAGE_CONNECTED_THRESHOLD ) && ( Get_Cell_Voltage(3) > VOLTAGE_CONNECTED_THRESHOLD )) {
		cell_connected_bitmask |= 0b1000;
	}
	else {
		cell_connected_bitmask &= ~0b1000;
	}
	if (( Get_Three_S_Voltage() > VOLTAGE_CONNECTED_THRESHOLD ) && ( Get_Cell_Voltage(2) > VOLTAGE_CONNECTED_THRESHOLD )) {
		cell_connected_bitmask |= 0b0100;
	}
	else {
		cell_connected_bitmask &= ~0b0100;
	}
	if (( Get_Two_S_Voltage() > VOLTAGE_CONNECTED_THRESHOLD ) && ( Get_Cell_Voltage(1) > VOLTAGE_CONNECTED_THRESHOLD )) {
		cell_connected_bitmask |= 0b0010;
	}
	else {
		cell_connected_bitmask &= ~0b0010;
	}
	if ( Get_Cell_Voltage(0) > VOLTAGE_CONNECTED_THRESHOLD ) {
		cell_connected_bitmask |= 0b0001;
	}
	else {
		cell_connected_bitmask &= ~0b0001;
	}

	if ( cell_connected_bitmask & (1<<3) ) {
		if ( (cell_connected_bitmask & THREE_S_BITMASK) == THREE_S_BITMASK ) {
			battery_state.number_of_cells = 4;
			Clear_Error_State(CELL_CONNECTION_ERROR);
		}
		else {
			battery_state.number_of_cells = 0;
			Set_Error_State(CELL_CONNECTION_ERROR);
		}
	}
	else if ( cell_connected_bitmask & (1<<2) ) {
		if ( (cell_connected_bitmask & TWO_S_BITMASK) ==  TWO_S_BITMASK ) {
			battery_state.number_of_cells = 3;
			Clear_Error_State(CELL_CONNECTION_ERROR);
		}
		else {
			battery_state.number_of_cells = 0;
			Set_Error_State(CELL_CONNECTION_ERROR);
		}
	}
	else if ( cell_connected_bitmask & (1<<1) ) {
		if ( (cell_connected_bitmask & ONE_S_BITMASK) == ONE_S_BITMASK ) {
			battery_state.number_of_cells = 2;
			Clear_Error_State(CELL_CONNECTION_ERROR);
		}
		else {
			battery_state.number_of_cells = 0;
			Set_Error_State(CELL_CONNECTION_ERROR);
		}
	}
	else {
		battery_state.number_of_cells = 0;
		Clear_Error_State(CELL_CONNECTION_ERROR);
	}

	if ( battery_state.number_of_cells > 1 ) {
		battery_state.balance_port_connected = CONNECTED;
	}
	else {
		battery_state.balance_port_connected = NOT_CONNECTED;
	}
}

/**
 * @brief Checks if any cell is over or under voltage
 */
void Cell_Voltage_Safety_Check()
{
	uint8_t over_voltage_temp = 0;
	uint8_t under_voltage_temp = 0;

	for (int i = 0; i < battery_state.number_of_cells; i++) {
		if (Get_Cell_Voltage(i) > CELL_OVER_VOLTAGE_DISABLE_CHARGING) {
			over_voltage_temp = 1;
		}

		if (Get_Cell_Voltage(i) < MIN_CELL_VOLTAGE_SAFE_LIMIT) {
			under_voltage_temp = 1;
		}
	}

	if (under_voltage_temp == 1) {
		Set_Error_State(CELL_VOLTAGE_ERROR);
	}
	else {
		Clear_Error_State(CELL_VOLTAGE_ERROR);
	}

	battery_state.cell_over_voltage = over_voltage_temp;
}

/**
 * @brief Determines the state of connections based on ADC readings
 */
void Battery_Connection_State()
{
	if ( Get_Battery_Voltage() > VOLTAGE_CONNECTED_THRESHOLD ) {
		battery_state.xt60_connected = CONNECTED;
	}
	else {
		battery_state.xt60_connected = NOT_CONNECTED;
	}



#if ENABLE_BALANCING
	Balance_Connection_State();
#else
	battery_state.balance_port_connected = CONNECTED;
	battery_state.number_of_cells = NUM_SERIES;
  Clear_Error_State(CELL_CONNECTION_ERROR);
#endif

	MCU_Temperature_Safety_Check();

#if ENABLE_BALANCING
	Cell_Voltage_Safety_Check();
#endif

#if ENABLE_BALANCING
	//Only update the balancing state if charging is off
	if (Get_Regulator_Charging_State() == 0) {
		Balance_Battery();
	}
#endif

	if ((battery_state.xt60_connected == CONNECTED) && (battery_state.balance_port_connected == CONNECTED)){
		if (Get_Battery_Voltage() < (battery_state.number_of_cells * CELL_VOLTAGE_TO_ENABLE_CHARGING)) {
			battery_state.requires_charging = 1;
		}
		else {
			battery_state.requires_charging = 0;
		}
	}
	else {
		battery_state.requires_charging = 0;
	}
}

/**
 * @brief Controls the GPIO outputs of the balancing circuit
 * @param  cell_balancing_gpio_bitmask: Four bit bitmask for cells 1-4. 1 balancing enabled, 0 disabled. Position 0 - cell 1, 1 - cell 2, etc.
 */
void Balancing_GPIO_Control(uint8_t cell_balancing_gpio_bitmask)
{
	if ( cell_balancing_gpio_bitmask & (1<<3) ) {
		HAL_GPIO_WritePin(CELL_4S_DIS_EN_GPIO_Port, CELL_4S_DIS_EN_Pin, GPIO_PIN_SET);
	}
	else {
		HAL_GPIO_WritePin(CELL_4S_DIS_EN_GPIO_Port, CELL_4S_DIS_EN_Pin, GPIO_PIN_RESET);
	}

	if ( cell_balancing_gpio_bitmask & (1<<2) ) {
		HAL_GPIO_WritePin(CELL_3S_DIS_EN_GPIO_Port, CELL_3S_DIS_EN_Pin, GPIO_PIN_SET);
	}
	else {
		HAL_GPIO_WritePin(CELL_3S_DIS_EN_GPIO_Port, CELL_3S_DIS_EN_Pin, GPIO_PIN_RESET);
	}

	if ( cell_balancing_gpio_bitmask & (1<<1) ) {
		HAL_GPIO_WritePin(CELL_2S_DIS_EN_GPIO_Port, CELL_2S_DIS_EN_Pin, GPIO_PIN_SET);
	}
	else {
		HAL_GPIO_WritePin(CELL_2S_DIS_EN_GPIO_Port, CELL_2S_DIS_EN_Pin, GPIO_PIN_RESET);
	}

	if ( cell_balancing_gpio_bitmask & (1<<0) ) {
		HAL_GPIO_WritePin(CELL_1S_DIS_EN_GPIO_Port, CELL_1S_DIS_EN_Pin, GPIO_PIN_SET);
	}
	else {
		HAL_GPIO_WritePin(CELL_1S_DIS_EN_GPIO_Port, CELL_1S_DIS_EN_Pin, GPIO_PIN_RESET);
	}
}

/**
 * @brief Checks if the MCU temperature is unsafe and sets battery error flag if so. Clears flag if temperature falls below recovery threshold.
 */
void MCU_Temperature_Safety_Check() {
	if (Get_MCU_Temperature() > MAX_MCU_TEMP_C_FOR_OPERATION) {
		Set_Error_State(MCU_OVER_TEMP);
	}
	else if ( ((Get_Error_State() & MCU_OVER_TEMP) == MCU_OVER_TEMP) && (Get_MCU_Temperature() < MCU_TEMP_C_RECOVERY) ) {
		Clear_Error_State(MCU_OVER_TEMP);
	}
}

/**
 * @brief Returns the balance connection state
 * @retval uint8_t CONNECTED or NOT_CONNECTED
 */
uint8_t Get_Balance_Connection_State()
{
	return battery_state.balance_port_connected;
}

/**
 * @brief Returns the state of balancing
 * @retval uint8_t battery_state.cell_balance_bitmask if balancing enabled or 0 if not enabled
 */
uint8_t Get_Balancing_State()
{
	if (battery_state.balancing_enabled == 1) {
		return battery_state.cell_balance_bitmask;
	}
	return 0;
}

/**
 * @brief Returns the state of charging
 * @retval uint8_t 1 if charging is required or 0 if not required
 */
uint8_t Get_Requires_Charging_State()
{
	return battery_state.requires_charging;
}

/**
 * @brief Returns the number of cells connected to the balance port
 * @retval uint8_t 2, 3, or 4
 */
uint8_t Get_Number_Of_Cells()
{
	return battery_state.number_of_cells;
}

/**
 * @brief Returns the XT60 connection state
 * @retval uint8_t CONNECTED or NOT_CONNECTED
 */
uint8_t Get_XT60_Connection_State()
{
	return battery_state.xt60_connected;
}

uint8_t Get_Cell_Over_Voltage_State()
{
	return battery_state.cell_over_voltage;
}

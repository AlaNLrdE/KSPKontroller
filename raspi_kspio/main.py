import RPi.GPIO as GPIO
from . import input_handler, output_handler, serialcoms, handshake, utilities

def main():
    GPIO.setmode(GPIO.BCM)
    output_handler.init_leds()
    input_handler.controls_init()
    output_handler.leds_all_off()

    ser = serialcoms.SerialComs()
    last_packet_time = utilities.millis()

    try:
        while True:
            # --- Input ---
            buttons = input_handler.read_buttons()
            # throttle = input_handler.read_throttle_adc()  # Enable when ADC is connected
            throttle = input_handler.read_throttle()

            # --- Serial Receive ---
            vessel_data = ser.read_vessel_data()
            if vessel_data:
                last_packet_time = utilities.millis()
                # Process vessel_data as needed

            # --- Output ---
            # Example: set GLED if SAS is active
            output_handler.set_led(output_handler.GLED, buttons['sas'])

            # Add more output logic as needed

    except KeyboardInterrupt:
        GPIO.cleanup()

if __name__ == "__main__":
    main()
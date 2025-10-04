import RPi.GPIO as GPIO
from .config import SASPIN, RCSPIN, CG1PIN, THROTTLEPIN, THROTTLEDB

def controls_init():
    GPIO.setup([SASPIN, RCSPIN, CG1PIN], GPIO.IN, pull_up_down=GPIO.PUD_UP)

def read_buttons():
    return {
        'sas': not GPIO.input(SASPIN),
        'rcs': not GPIO.input(RCSPIN),
        'cg1': not GPIO.input(CG1PIN)
    }

# Analog input (ADC) - disabled by default
# def read_throttle_adc():
#     # Example for MCP3008 via spidev
#     import spidev
#     spi = spidev.SpiDev()
#     spi.open(0, 0)
#     adc = spi.xfer2([1, (8 + THROTTLEPIN) << 4, 0])
#     value = ((adc[1] & 3) << 8) + adc[2]
#     spi.close()
#     return value

def read_throttle():
    # Placeholder: return 0 if ADC not connected
    return 0
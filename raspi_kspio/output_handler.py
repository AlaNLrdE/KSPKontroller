import RPi.GPIO as GPIO
from .config import GLED, YLED, RLED, SASLED, RCSLED, CG1LED

def init_leds():
    GPIO.setup([GLED, YLED, RLED, SASLED, RCSLED, CG1LED], GPIO.OUT)
    leds_all_off()

def leds_all_off():
    for pin in [GLED, YLED, RLED, SASLED, RCSLED, CG1LED]:
        GPIO.output(pin, GPIO.LOW)

def set_led(pin, state):
    GPIO.output(pin, GPIO.HIGH if state else GPIO.LOW)
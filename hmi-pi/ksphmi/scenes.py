"""Scene context supplied by KSPSerialIO; independent of the flight telemetry link."""
import socket

# Each view has a distinct purpose and three task cards. No simulated mission data is shown live.
VIEWS = {
    "space_center": ("Space Center", "MISSION OPERATIONS", (
        ("BUILD", "VAB / Spaceplane Hangar", "Design the next mission vehicle"),
        ("PLAN", "Mission Control", "Review contracts and their requirements"),
        ("PREPARE", "Astronaut Complex", "Recruit and assign your crew"))),
    "vab": ("Vehicle Assembly Building", "ROCKET PREPARATION", (
        ("01", "Payload & crew", "Check capacity, crew and mission equipment"),
        ("02", "Propulsion & staging", "Review delta-v, thrust and decouplers"),
        ("03", "Launch readiness", "Check parachutes, power and communications"))),
    "sph": ("Spaceplane Hangar", "AIRCRAFT PREPARATION", (
        ("01", "Balance", "Compare centre of mass and centre of lift"),
        ("02", "Flight controls", "Check control surfaces and action groups"),
        ("03", "Runway readiness", "Review landing gear, brakes and fuel"))),
    "mission_control": ("Mission Control", "CONTRACT PLANNING", (
        ("OBJECTIVE", "Mission requirements", "Review every parameter before accepting"),
        ("SCHEDULE", "Deadline & destination", "Allow time for transfer and completion"),
        ("BUDGET", "Rewards & penalties", "Compare advance, payout and launch cost"))),
    "astronaut_complex": ("Astronaut Complex", "CREW OPERATIONS", (
        ("PILOT", "Flight control", "Review pilot availability and experience"),
        ("ENGINEER", "Vehicle support", "Plan repairs and surface operations"),
        ("SCIENTIST", "Science operations", "Choose crew for experiments and laboratories"))),
    "research": ("Research & Development", "TECHNOLOGY PLANNING", (
        ("SCIENCE", "Research budget", "Review available science before unlocking nodes"),
        ("TECHNOLOGY", "Mission capabilities", "Prioritize parts needed for the next mission"),
        ("EXPERIMENTS", "Science archives", "Review results and unexplored destinations"))),
    "administration": ("Administration", "PROGRAM STRATEGY", (
        ("STRATEGY", "Active policies", "Review commitments and resource conversion"),
        ("RESOURCES", "Program priorities", "Balance funds, science and reputation"),
        ("PLANNING", "Long-term goals", "Choose policies that support upcoming missions"))),
    "tracking_station": ("Tracking Station", "FLEET OPERATIONS", (
        ("TRACK", "Active missions", "Select a vessel and inspect its trajectory"),
        ("PLAN", "Transfers & encounters", "Review destinations and sphere changes"),
        ("CONTROL", "Resume a mission", "Switch to a vessel for live flight telemetry"))),
}


def read_context(address, port=50002):
    """One bounded request. An older plugin or blocked UDP port leaves the generic waiting view."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(0.15)
            sock.connect((address, port))
            sock.send(b"KSPHMI/1")
            reply = sock.recv(256).decode("ascii")
        prefix, scene = reply.split(" ", 1)
        return scene if prefix == "KSPHMI/1" and scene in VIEWS else ""
    except (OSError, ValueError, UnicodeError):
        return ""

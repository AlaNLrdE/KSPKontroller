FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev \
    fonts-freefont-ttf python3 python3-venv python3-tk ca-certificates \
    xvfb x11-utils openbox x11vnc novnc websockify wmctrl \
    && rm -rf /var/lib/apt/lists/*
COPY tools/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/kspsim \
    && /opt/kspsim/bin/pip install --no-cache-dir -r /tmp/requirements.txt
ENV KSPSIM_PYTHON=/opt/kspsim/bin/python
ENV SDL_VIDEODRIVER=dummy
ENV SDL_AUDIODRIVER=dummy
ENV KSPHMI_LOCAL_CONFIG=/dev/null
COPY tools/hmi-container/run.sh /opt/run-hmi.sh
ENTRYPOINT ["bash", "/opt/run-hmi.sh"]

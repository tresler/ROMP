#!/bin/bash
# Pretty installer for own-vp

set -e

echo "------------------------------------------------"
echo "    ROMP: Raspberry Pi OSC Media Player         "
echo "------------------------------------------------"

ROMP_DATA_DIR="$HOME/.romp"

# 0. Prepare data directory
echo "Preparing data directories at $ROMP_DATA_DIR..."
mkdir -p "$ROMP_DATA_DIR"
mkdir -p "$ROMP_DATA_DIR/media"

# Migrate setup.txt if exists locally and not in destination
if [ -f "setup.txt" ]; then
    if [ ! -f "$ROMP_DATA_DIR/setup.txt" ]; then
        echo "Copying setup.txt to $ROMP_DATA_DIR..."
        cp setup.txt "$ROMP_DATA_DIR/"
    fi
fi

# Migrate media folder if exists locally and not in destination
if [ -d "media" ] && [ ! -d "$ROMP_DATA_DIR/media" ]; then
    echo "Migrating media folder to $ROMP_DATA_DIR..."
    cp -r media/* "$ROMP_DATA_DIR/media/" 2>/dev/null || true
fi

HAS_MEDIA=true
if [ -z "$(ls -A $ROMP_DATA_DIR/media)" ]; then
    HAS_MEDIA=false
fi

# 1. Install dependencies
sudo ./install_dependencies.sh

# 2. Compile
echo "Compiling the project..."
make clean
make -j$(nproc)

# 3. Install binary
echo "Installing to /usr/local/bin..."
sudo make install

# 4. Create alias
if ! grep -q "alias romp" ~/.bashrc; then
    echo "Adding alias to ~/.bashrc..."
    echo "alias romp='/usr/local/bin/romp'" >> ~/.bashrc
fi

# 5. Setup systemd service (Auto-start)
read -p "Do you want to enable auto-start on boot? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    SERVICE_FILE="/etc/systemd/system/romp.service"
    CURRENT_USER=$(whoami)
    
    echo "Creating systemd service for user $CURRENT_USER..."
    sudo bash -c "cat > $SERVICE_FILE" <<EOF
[Unit]
Description=ROMP - Raspberry Pi OSC Media Player
After=network.target

[Service]
ExecStart=/usr/local/bin/romp
WorkingDirectory=$ROMP_DATA_DIR
User=$CURRENT_USER
Restart=always
RestartSec=5
Environment=SDL_VIDEODRIVER=kmsdrm

[Install]
WantedBy=multi-user.target
EOF

    sudo systemctl daemon-reload
    sudo systemctl enable romp.service
    echo "Service enabled. It will start on next boot."
    echo "To start now: sudo systemctl start romp"
fi

echo "------------------------------------------------"
echo "Installation finished successfully!"
echo "You can now run 'romp' from your terminal."
if [ "$HAS_MEDIA" = false ]; then
    echo -e "\e[31m\e[1mIMPORTANT: The media folder is empty!\e[0m"
    echo -e "\e[31mPlease move your videos, images and audio to: $ROMP_DATA_DIR/media/\e[0m"
fi
echo "------------------------------------------------"
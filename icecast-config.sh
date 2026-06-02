#!/usr/bin/env bash
# icecast-bridge-config.sh
# Interactively builds the Icecast Bridge plugin JSON block for trunk-recorder.
# Outputs to stdout or a file. Run with: bash icecast-bridge-config.sh

set -euo pipefail

# ---- helpers ----------------------------------------------------------------

ask() {
    local prompt="$1"
    local default="${2:-}"
    local value
    if [[ -n "$default" ]]; then
        read -rp "$prompt [$default]: " value
        echo "${value:-$default}"
    else
        read -rp "$prompt: " value
        echo "$value"
    fi
}

ask_yn() {
    local prompt="$1"
    local default="${2:-n}"
    local value
    read -rp "$prompt [y/n, default=${default}]: " value
    value="${value:-$default}"
    [[ "$value" =~ ^[Yy] ]] && echo "true" || echo "false"
}

ask_int() {
    local prompt="$1"
    local default="$2"
    local value
    while true; do
        read -rp "$prompt [$default]: " value
        value="${value:-$default}"
        if [[ "$value" =~ ^[0-9]+$ ]]; then
            echo "$value"
            return
        fi
        echo "  Please enter a whole number." >&2
    done
}

ask_float() {
    local prompt="$1"
    local default="$2"
    local value
    while true; do
        read -rp "$prompt [$default]: " value
        value="${value:-$default}"
        if [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
            echo "$value"
            return
        fi
        echo "  Please enter a number (e.g. 1.0, 4.0)." >&2
    done
}

# JSON-escape a string (handles quotes and backslashes)
json_str() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    echo "$s"
}

# ---- header -----------------------------------------------------------------

echo ""
echo "=================================================="
echo "  Icecast Bridge Plugin Config Builder"
echo "=================================================="
echo ""
echo "This script builds the plugin JSON block for trunk-recorder's config.json."
echo "You can run it multiple times to add additional plugin instances."
echo ""

# ---- server connection ------------------------------------------------------

echo "--- Server Connection ---"
host=$(ask "Icecast server host" "localhost")
port=$(ask_int "Icecast server port" "8000")
source_user=$(ask "Source username" "source")
source_password=$(ask "Source password" "hackme")

echo ""
echo "--- Admin Credentials (for metadata updates) ---"
echo "  These must match <admin-user> and <admin-password> in icecast.xml."
echo "  Leave blank to use the source password as the admin password."
admin_user=$(ask "Admin username" "admin")
admin_password_input=$(ask "Admin password (blank = same as source password)" "")
if [[ -z "$admin_password_input" ]]; then
    admin_password="$source_password"
else
    admin_password="$admin_password_input"
fi

# ---- library ----------------------------------------------------------------

echo ""
echo "--- Plugin Library ---"
library=$(ask "Path to plugin .so" "/usr/local/lib/trunk-recorder/libtr_plugin_icecast.so")

# ---- mounts -----------------------------------------------------------------

echo ""
echo "--- Mounts ---"
echo "  Each mount is one stream. You can add multiple."
echo ""

mounts_json=""
mount_names=()

mount_index=0
while true; do
    mount_index=$((mount_index + 1))
    echo "  Mount #${mount_index}:"

    mount_path=$(ask "    Mountpoint (e.g. /dispatch.mp3)")
    mount_names+=("$mount_path")

    mount_display_name=$(ask "    Display name" "${mount_path}")
    mount_description=$(ask "    Description" "Scanner audio")
    mount_genre=$(ask "    Genre" "Public Safety")
    mount_public=$(ask_yn "    Public (listed in Icecast directory)?" "n")
    mount_sample_rate=$(ask_int "    Sample rate (Hz)" "22050")
    mount_bitrate=$(ask_int "    Bitrate (kbps)" "64")
    mount_channels=$(ask_int "    Channels (1=mono, 2=stereo)" "1")
    mount_gain=$(ask_float "    Gain (1.0=unity, 2.0=+6dB)" "1.0")
    mount_standby=$(ask "    Standby title (shown when no call active)" "Standby")
    mount_legacy=$(ask_yn "    Legacy SOURCE mode? (required for Broadcastify)" "n")

    if [[ "$mount_legacy" == "true" ]]; then
        mount_icy=$(ask_int "    ICY metaint (in-band metadata interval bytes, 0=off)" "8192")
        mount_admin_user=""
        mount_admin_password=""
    else
        mount_icy=$(ask_int "    ICY metaint (in-band metadata interval bytes, 0=off)" "0")
        mount_admin_user="$admin_user"
        mount_admin_password="$admin_password"
    fi

    mount_meta_format=$(ask "    Metadata format" "TG: {talkgroup_tag} ({talkgroup}) {talker_alias} {time}")

    # Build mount JSON
    mount_json="      {
        \"mount\": \"$(json_str "$mount_path")\",
        \"name\": \"$(json_str "$mount_display_name")\",
        \"description\": \"$(json_str "$mount_description")\",
        \"genre\": \"$(json_str "$mount_genre")\",
        \"public\": $mount_public,
        \"sample_rate\": $mount_sample_rate,
        \"bitrate\": $mount_bitrate,
        \"channels\": $mount_channels,
        \"gain\": $mount_gain,
        \"legacy_source\": $mount_legacy,
        \"icy_metaint\": $mount_icy,
        \"metadata_format\": \"$(json_str "$mount_meta_format")\",
        \"metadata_standby\": \"$(json_str "$mount_standby")\""

    if [[ -n "$mount_admin_user" ]]; then
        mount_json="${mount_json},
        \"admin_user\": \"$(json_str "$mount_admin_user")\",
        \"admin_password\": \"$(json_str "$mount_admin_password")\""
    fi

    mount_json="${mount_json}
      }"

    if [[ -z "$mounts_json" ]]; then
        mounts_json="$mount_json"
    else
        mounts_json="${mounts_json},
${mount_json}"
    fi

    echo ""
    add_another=$(ask_yn "  Add another mount?" "n")
    [[ "$add_another" == "false" ]] && break
    echo ""
done

# ---- systems ----------------------------------------------------------------

echo ""
echo "--- System Talkgroup Mappings ---"
echo "  Map talkgroups to mounts. Available mounts:"
for m in "${mount_names[@]}"; do
    echo "    $m"
done
echo ""

systems_json=""
sys_index=0

while true; do
    sys_index=$((sys_index + 1))
    echo "  System #${sys_index}:"
    sys_short=$(ask "    shortName (must match system in TR config)")

    tg_json=""
    tg_index=0

    echo ""
    echo "    Talkgroup entry method:"
    echo "      1. Enter talkgroups manually one by one"
    echo "      2. Import from a file (one TGID per line)"
    tg_method=$(ask_int "    Choose" "1")

    if [[ "$tg_method" == "2" ]]; then
        # ---- file import ----
        while true; do
            tg_file=$(ask "    Path to talkgroup file")
            if [[ ! -f "$tg_file" ]]; then
                echo "    Error: file not found: $tg_file" >&2
                continue
            fi
            break
        done

        # Assign all TGIDs to one mount or pick per-mount
        if [[ ${#mount_names[@]} -eq 1 ]]; then
            tg_mount="${mount_names[0]}"
            echo "    All talkgroups will be assigned to ${tg_mount}"
        else
            echo "    Assign all talkgroups to one mount, or map each individually?"
            echo "      1. Assign all to one mount"
            echo "      2. Map each talkgroup to a mount individually"
            assign_mode=$(ask_int "    Choose" "1")

            if [[ "$assign_mode" == "1" ]]; then
                echo "    Available mounts:"
                for i in "${!mount_names[@]}"; do
                    echo "      $((i+1)). ${mount_names[$i]}"
                done
                mount_choice=$(ask_int "    Mount number" "1")
                tg_mount="${mount_names[$((mount_choice-1))]}"
            else
                tg_mount=""  # per-line assignment below
            fi
        fi

        # Read the file — strip comments (#), blank lines, and whitespace
        imported=0
        skipped=0
        while IFS= read -r line || [[ -n "$line" ]]; do
            # Strip inline comments and whitespace
            tgid="${line%%#*}"
            tgid="${tgid//[[:space:]]/}"
            [[ -z "$tgid" ]] && continue

            # Validate numeric
            if ! [[ "$tgid" =~ ^[0-9]+$ ]]; then
                echo "    Skipping non-numeric value: $tgid" >&2
                skipped=$((skipped + 1))
                continue
            fi

            # Per-talkgroup mount assignment if needed
            if [[ -z "$tg_mount" ]]; then
                echo "    Available mounts:"
                for i in "${!mount_names[@]}"; do
                    echo "      $((i+1)). ${mount_names[$i]}"
                done
                mount_choice=$(ask_int "    TGID ${tgid} -> mount number" "1")
                line_mount="${mount_names[$((mount_choice-1))]}"
            else
                line_mount="$tg_mount"
            fi

            entry="        \"${tgid}\": \"$(json_str "$line_mount")\""
            if [[ -z "$tg_json" ]]; then
                tg_json="$entry"
            else
                tg_json="${tg_json},
${entry}"
            fi
            imported=$((imported + 1))
        done < "$tg_file"

        echo "    Imported ${imported} talkgroup(s)${skipped:+, skipped ${skipped} invalid}."

        # Offer to add more manually on top of the import
        echo ""
        add_more=$(ask_yn "    Add additional talkgroups manually?" "n")
        if [[ "$add_more" == "true" ]]; then
            echo "    Enter additional talkgroups (blank TGID to stop):"
            while true; do
                tgid=$(ask "      TGID (blank to finish)" "")
                [[ -z "$tgid" ]] && break

                if [[ ${#mount_names[@]} -eq 1 ]]; then
                    line_mount="${mount_names[0]}"
                    echo "      -> auto-assigned to ${line_mount}"
                else
                    echo "      Available mounts:"
                    for i in "${!mount_names[@]}"; do
                        echo "        $((i+1)). ${mount_names[$i]}"
                    done
                    mount_choice=$(ask_int "      Mount number" "1")
                    line_mount="${mount_names[$((mount_choice-1))]}"
                fi

                entry="        \"${tgid}\": \"$(json_str "$line_mount")\""
                if [[ -z "$tg_json" ]]; then
                    tg_json="$entry"
                else
                    tg_json="${tg_json},
${entry}"
                fi
            done
        fi

    else
        # ---- manual entry ----
        echo "    Talkgroup mappings (enter blank TGID to stop):"
        while true; do
            tgid=$(ask "      TGID (blank to finish)" "")
            [[ -z "$tgid" ]] && break

            if [[ ${#mount_names[@]} -eq 1 ]]; then
                tg_mount="${mount_names[0]}"
                echo "      -> auto-assigned to ${tg_mount}"
            else
                echo "      Available mounts:"
                for i in "${!mount_names[@]}"; do
                    echo "        $((i+1)). ${mount_names[$i]}"
                done
                mount_choice=$(ask_int "      Mount number" "1")
                tg_mount="${mount_names[$((mount_choice-1))]}"
            fi

            entry="        \"${tgid}\": \"$(json_str "$tg_mount")\""
            if [[ -z "$tg_json" ]]; then
                tg_json="$entry"
            else
                tg_json="${tg_json},
${entry}"
            fi
            tg_index=$((tg_index + 1))
        done
    fi

    sys_json="      {
        \"shortName\": \"$(json_str "$sys_short")\",
        \"talkgroups\": {
${tg_json}
        }
      }"

    if [[ -z "$systems_json" ]]; then
        systems_json="$sys_json"
    else
        systems_json="${systems_json},
${sys_json}"
    fi

    echo ""
    add_sys=$(ask_yn "  Add another system?" "n")
    [[ "$add_sys" == "false" ]] && break
    echo ""
done

# ---- assemble ---------------------------------------------------------------

plugin_json="  {
    \"name\": \"Icecast Bridge\",
    \"library\": \"$(json_str "$library")\",
    \"host\": \"$(json_str "$host")\",
    \"port\": $port,
    \"source_user\": \"$(json_str "$source_user")\",
    \"source_password\": \"$(json_str "$source_password")\",
    \"mounts\": [
${mounts_json}
    ],
    \"systems\": [
${systems_json}
    ]
  }"

# ---- output -----------------------------------------------------------------

echo ""
echo "=================================================="
echo "  Generated Plugin JSON"
echo "=================================================="
echo ""
echo "$plugin_json"
echo ""

echo "=================================================="
echo "  Output Options"
echo "=================================================="
echo ""
echo "  1. Print only (already done above)"
echo "  2. Save plugin block to a standalone JSON file"
echo "  3. Inject directly into an existing config.json using jq"
echo ""
output_choice=$(ask_int "Choose an option" "1")

case "$output_choice" in
    2)
        outfile=$(ask "Output filename" "icecast-bridge-plugin.json")
        echo "$plugin_json" > "$outfile"
        echo "Saved to $outfile"
        ;;
    3)
        # Check jq is available
        if ! command -v jq &>/dev/null; then
            echo ""
            echo "Error: jq is not installed. Install it with:"
            echo "  sudo apt install jq"
            exit 1
        fi

        # Ask for config.json path
        config_file=$(ask "Path to config.json" "config.json")

        if [[ ! -f "$config_file" ]]; then
            echo "Error: $config_file not found."
            exit 1
        fi

        # Validate it's valid JSON
        if ! jq empty "$config_file" 2>/dev/null; then
            echo "Error: $config_file is not valid JSON."
            exit 1
        fi

        # Check if plugins array exists
        has_plugins=$(jq 'has("plugins")' "$config_file")

        # Back up the config first
        backup="${config_file}.bak.$(date +%Y%m%d%H%M%S)"
        cp "$config_file" "$backup"
        echo "Backup saved to $backup"

        # Parse the plugin JSON into a jq-compatible temp file
        tmpfile=$(mktemp /tmp/icecast-plugin-XXXXXX.json)
        echo "$plugin_json" > "$tmpfile"

        if [[ "$has_plugins" == "true" ]]; then
            # Check if an Icecast Bridge plugin block already exists
            existing=$(jq '[.plugins[] | select(.name == "Icecast Bridge")] | length' "$config_file")
            if [[ "$existing" -gt 0 ]]; then
                echo ""
                echo "Warning: config.json already contains an 'Icecast Bridge' plugin block."
                replace=$(ask_yn "Replace the existing block?" "n")
                if [[ "$replace" == "true" ]]; then
                    # Replace the first matching block
                    jq --slurpfile new "$tmpfile" \
                        '(.plugins[] | select(.name == "Icecast Bridge")) |= $new[0]' \
                        "$config_file" > "${config_file}.tmp" && mv "${config_file}.tmp" "$config_file"
                    echo "Replaced existing Icecast Bridge block in $config_file"
                else
                    # Append as additional instance
                    jq --slurpfile new "$tmpfile" \
                        '.plugins += [$new[0]]' \
                        "$config_file" > "${config_file}.tmp" && mv "${config_file}.tmp" "$config_file"
                    echo "Appended new Icecast Bridge block to $config_file"
                fi
            else
                # Append to existing plugins array
                jq --slurpfile new "$tmpfile" \
                    '.plugins += [$new[0]]' \
                    "$config_file" > "${config_file}.tmp" && mv "${config_file}.tmp" "$config_file"
                echo "Added Icecast Bridge plugin to $config_file"
            fi
        else
            # No plugins array — create it
            jq --slurpfile new "$tmpfile" \
                '. + {"plugins": [$new[0]]}' \
                "$config_file" > "${config_file}.tmp" && mv "${config_file}.tmp" "$config_file"
            echo "Created plugins array and added Icecast Bridge plugin to $config_file"
        fi

        rm -f "$tmpfile"

        # Validate the result
        if jq empty "$config_file" 2>/dev/null; then
            echo "config.json updated successfully."
        else
            echo "Error: resulting config.json is invalid JSON. Restoring backup."
            cp "$backup" "$config_file"
            exit 1
        fi
        ;;
    *)
        # Option 1 or anything else — already printed above
        ;;
esac

echo ""
echo "Done."

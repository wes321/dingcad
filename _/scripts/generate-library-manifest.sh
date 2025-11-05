#!/bin/bash
# Generate library-manifest.json from library directory structure
# Outputs to _/config/library-manifest.json
# Scans both library/ and _/library/ directories

# Get the directory where this script is located, then go to repo root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT" || exit 1

LIBRARY_DIRS=("library" "_/library")
MANIFEST_FILE="_/config/library-manifest.json"
EXCLUDE_FOLDERS="util"

# Ensure config directory exists
mkdir -p "$(dirname "$MANIFEST_FILE")"

# Start JSON
echo '{' > "$MANIFEST_FILE"
echo '  "files": [' >> "$MANIFEST_FILE"

first=true

# Collect all files first, then process them
TEMP_FILE=$(mktemp)
for LIBRARY_DIR in "${LIBRARY_DIRS[@]}"; do
    if [ ! -d "$LIBRARY_DIR" ]; then
        continue
    fi
    
    # Find all .js files in library directory, excluding util folder
    find "$LIBRARY_DIR" -name "*.js" -type f ! -path "*/util/*" >> "$TEMP_FILE"
done

# Sort all files and process them
sort "$TEMP_FILE" | while read -r file; do
    # Determine which library directory this file belongs to
    LIBRARY_DIR=""
    if [[ "$file" == _/library/* ]]; then
        LIBRARY_DIR="_/library"
        rel_path="${file#_/library/}"
    elif [[ "$file" == library/* ]]; then
        LIBRARY_DIR="library"
        rel_path="${file#library/}"
    else
        continue
    fi
    
    # Extract folder and filename
    if [[ "$rel_path" == */* ]]; then
        folder="${rel_path%%/*}"
        filename="${rel_path##*/}"
    else
        folder=""
        filename="$rel_path"
    fi
    
    # Generate description from filename (remove .js, replace underscores with spaces)
    desc="${filename%.js}"
    desc="${desc//_/ }"  # Replace underscores with spaces
    # Capitalize first letter (portable method)
    first_char=$(echo "$desc" | cut -c1 | tr '[:lower:]' '[:upper:]')
    rest_chars=$(echo "$desc" | cut -c2-)
    desc="$first_char$rest_chars"
    
    # Add comma if not first item
    if [ "$first" = true ]; then
        first=false
    else
        echo ',' >> "$MANIFEST_FILE"
    fi
    
    # Write file entry
    echo -n '    {' >> "$MANIFEST_FILE"
    echo -n "\"name\": \"$filename\"," >> "$MANIFEST_FILE"
    echo -n " \"desc\": \"$desc\"," >> "$MANIFEST_FILE"
    echo -n " \"path\": \"./$file\"," >> "$MANIFEST_FILE"
    echo -n " \"folder\": \"$folder\"" >> "$MANIFEST_FILE"
    echo -n '}' >> "$MANIFEST_FILE"
done

rm -f "$TEMP_FILE"

echo '' >> "$MANIFEST_FILE"
echo '  ],' >> "$MANIFEST_FILE"
echo "  \"excludeFolders\": [\"$EXCLUDE_FOLDERS\"]" >> "$MANIFEST_FILE"
echo '}' >> "$MANIFEST_FILE"

echo "✓ Generated $MANIFEST_FILE"


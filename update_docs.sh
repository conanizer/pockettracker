#!/bin/bash

# Navigate to project root
cd /path/to/PocketTracker

# Backup existing files
echo "📦 Creating backups..."
cp MVP_ROADMAP.md MVP_ROADMAP.md.backup
cp DEVELOPMENT_STATUS.md DEVELOPMENT_STATUS.md.backup
cp README.md README.md.backup
cp CLAUDE.md CLAUDE.md.backup

# Copy new versions
echo "📝 Copying updated files..."
cp /mnt/user-data/outputs/MVP_ROADMAP_FINAL.md ./MVP_ROADMAP.md
cp /mnt/user-data/outputs/DEVELOPMENT_STATUS_FINAL.md ./DEVELOPMENT_STATUS.md
cp /mnt/user-data/outputs/README_FINAL.md ./README.md
cp /mnt/user-data/outputs/CLAUDE_FINAL.md ./CLAUDE.md

# Create docs folder and add new file
mkdir -p docs
cp /mnt/user-data/outputs/REFACTORING_SIMPLE_EXPLANATION.md ./docs/

echo "✅ Core files updated!"
echo "⚠️  Manual updates still needed:"
echo "   - REFACTORING_ROADMAP.md (Phase 4 from REFACTORING_PHASE4_COMPLETE.md)"
echo "   - TECHNICAL_ARCHITECTURE.md (add finalization section)"
echo "   - PRODUCT_VISION.md (add copy/paste, update devices)"
echo "   - PROJECT_SUMMARY.md (update decision, devices)"
echo ""
echo "📖 See instructions above for what to add to each file"
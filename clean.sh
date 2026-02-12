#!/bin/bash
# Clean build artifacts

# Set JAVA_HOME if not set
if [ -z "$JAVA_HOME" ]; then
    export JAVA_HOME="/c/Program Files/Android/Android Studio/jbr"
fi

echo "🧹 Cleaning PocketTracker build artifacts..."
./gradlew clean

if [ $? -eq 0 ]; then
    echo "✅ Clean successful!"
else
    echo "❌ Clean failed!"
    exit 1
fi

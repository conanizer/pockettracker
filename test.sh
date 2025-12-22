#!/bin/bash
# Quick test script - compile check only (no unit tests yet)

# Set JAVA_HOME if not set
if [ -z "$JAVA_HOME" ]; then
    export JAVA_HOME="/c/Program Files/Android/Android Studio/jbr"
fi

echo "🧪 Testing PocketTracker..."
echo "📋 Running compile check..."

./gradlew compileDebugKotlin

if [ $? -eq 0 ]; then
    echo "✅ Compile check passed!"
else
    echo "❌ Compile check failed!"
    exit 1
fi

# TODO: Add smoke tests when test suite is ready
# echo "📋 Running unit tests..."
# ./gradlew test

#!/bin/sh
# This toy example uses wget and catch mode to act
# as a drag-and-drop download tool.

../wdraft catch | while read url
do
    wget "$url"
done

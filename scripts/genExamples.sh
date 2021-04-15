for SRC_FILE in examples/*.pizza; do
  LL_FILE=$(echo $SRC_FILE | sed 's/.pizza/.ll/' | sed 's/examples\//build\/out\//')
  JSON_FILE=$(echo $SRC_FILE | sed 's/.pizza/AST.json/' | sed 's/examples\//build\/out\//')
  echo "$SRC_FILE"
  ./build/bin/bake $SRC_FILE $JSON_FILE $LL_FILE > /dev/null
done

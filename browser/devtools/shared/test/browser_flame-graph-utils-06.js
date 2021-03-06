/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

// Tests that the text displayed is the function name, file name and line number
// if applicable.

let {FlameGraphUtils, FLAME_GRAPH_BLOCK_HEIGHT} = devtools.require("devtools/shared/widgets/FlameGraph");

add_task(function*() {
  yield promiseTab("about:blank");
  yield performTest();
  gBrowser.removeCurrentTab();
});

function* performTest() {
  let out = FlameGraphUtils.createFlameGraphDataFromSamples(TEST_DATA, {
    flattenRecursion: true
  });

  ok(out, "Some data was outputted properly");
  is(out.length, 10, "The outputted length is correct.");

  info("Got flame graph data:\n" + out.toSource() + "\n");

  for (let i = 0; i < out.length; i++) {
    let found = out[i];
    let expected = EXPECTED_OUTPUT[i];

    is(found.blocks.length, expected.blocks.length,
      "The correct number of blocks were found in this bucket.");

    for (let j = 0; j < found.blocks.length; j++) {
      is(found.blocks[j].x, expected.blocks[j].x,
        "The expected block X position is correct for this frame.");
      is(found.blocks[j].y, expected.blocks[j].y,
        "The expected block Y position is correct for this frame.");
      is(found.blocks[j].width, expected.blocks[j].width,
        "The expected block width is correct for this frame.");
      is(found.blocks[j].height, expected.blocks[j].height,
        "The expected block height is correct for this frame.");
      is(found.blocks[j].text, expected.blocks[j].text,
        "The expected block text is correct for this frame.");
    }
  }
}

let TEST_DATA = [{
  frames: [{
    location: "A (http://path/to/file.js:10:5"
  }, {
    location: "B (http://path/to/file.js:100:5"
  }],
  time: 50,
}];

let EXPECTED_OUTPUT = [{
  blocks: []
}, {
  blocks: []
}, {
  blocks: []
}, {
  blocks: []
}, {
  blocks: [{
    srcData: {
      startTime: 0,
      rawLocation: "A (http://path/to/file.js:10:5)"
    },
    x: 0,
    y: 0,
    width: 50,
    height: FLAME_GRAPH_BLOCK_HEIGHT,
    text: "A (file.js:10)"
  }]
}, {
  blocks: []
}, {
  blocks: []
}, {
  blocks: []
}, {
  blocks: []
}, {
  blocks: []
}];

const hactool = require('./build/Release/node-hactool.node');

const nodeHactool = {
  information: (arguments) => {
    const result = hactool.run(...arguments);

    return JSON.parse(result);
  },
  extract: (arguments) => {
    const result = hactool.run(...arguments);

    return JSON.parse(result);
  },
};

module.exports = nodeHactool;

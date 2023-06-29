const hactool = require('./build/Release/node-hactool.node');

const nodeHactool = {
  information: (options) => {
    if (typeof options?.source === 'undefined') {
      return {
        error: true,
        errorMessage: 'Provide a source file using the "source" option.',
      };
    }

    const parameters = [
      'hactool',
      '--info',
      '--intype',
      'pfs0',
    ];

    parameters.push(options.source);

    try {
      return JSON.parse(hactool.run(...parameters));
    } catch (error) {
      return {
        error: true,
        errorMessage: error.message,
      };
    }
  },
  extract: (args) => {
    const result = hactool.run(...args);

    return JSON.parse(result);
  },
};

module.exports = nodeHactool;

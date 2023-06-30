const hactool = require('./build/Release/node-hactool.node');

const inputTypes = [
  'nca',
  'pfs0',
  'xci',
  'romfs',
  'hfs0',
  'npdm',
  'pk11',
  'pk21',
  'ini1',
  'kip1',
  'nax0',
  'save',
  'keygen',
];

const nodeHactool = {
  error: function(errorMessage) {
    return {
      error: true,
      errorMessage,
    };
  },
  run: function(options) {
    // Default the input file type to pfs0 (nsp).
    let inputType = options.type ?? 'pfs0';

    // The type may not have been provided by the user.
    const userProvidedType = typeof options?.inputType !== 'undefined';

    // If we were given an input type make sure that it is one that could be processed.
    if (userProvidedType && !inputTypes.includes(inputType)) {
      return this.error(`The "${inputType}" input type is not a recognized input type.`);
    }

    // Make sure that the user provided a source file to process.
    if (typeof options?.source === 'undefined') {
      return this.error('Provide a source file using the "source" option.');
    }

    const parameters = [
      'hactool',
      options.action,
    ];

    if (userProvidedType) {
      // Process only the type provided by the user.
      try {
        return JSON.parse(hactool.run(...[...parameters, '--intype', inputType, options.source]));
      } catch (error) {
        // Convert Napi::Error exceptions.
        return this.error(error.message);
      }
    } else {
      let results = {
        warnings: [],
      };

      // The user did not specify an input type so try each type until we do not receive a type error.
      inputTypes.some((type) => {
        let check = { error: true };

        try {
          check = JSON.parse(hactool.run(...[...parameters, '--intype', type, options.source]));
        } catch (error) {
          if (error instanceof TypeError) {
            // There was a type error. This means the inputtype we just chose is incorrect.
            // Append the message to the list of warnings then try the next file type.
            results.warnings.push(`[${type}] ${error.message}`);
          } else {
            // There was an error, but not related to the filetype.
            // This means the input type is correct, but an error occurred.
            results = { ...this.error(error.message), ...results };

            return true;
          }
        }

        if (!check.error) {
          // We received a valid response without an error.

          // Combine the warnings from trying different file types as well as the warnings from the tool.
          const warnings = [...results.warnings, check.warnings];
          results = check;
          results.warnings = warnings;

          // Break out of the some loop.
          return true;
        }
      });

      return results;
    }
  },
  information: function(options) {
    return this.run({ ...options, action: '--info' });
  },
  extract: function(options) {
    return this.run({ ...options, action: '--file' });
  },
};

module.exports = nodeHactool;

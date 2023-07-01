const fs = require('fs');
const path = require('path');

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
  run: function(options, parameters) {
    // Default the input file type to pfs0 (nsp).
    let inputType = options?.type ?? 'pfs0';

    // The type may not have been provided by the user.
    const userProvidedType = typeof options?.type !== 'undefined';

    // If we were given an input type make sure that it is one that could be processed.
    if (userProvidedType && !inputTypes.includes(inputType)) {
      return this.error(`The "${inputType}" input type is not a recognized input type.`);
    }

    // Make sure that the user provided a source file to process.
    if (typeof options?.source === 'undefined' || typeof options.source !== 'string') {
      return this.error('Provide a source file using the "source" option.');
    }

    // Make sure that the source file is readable.
    try {
      fs.accessSync(options.source, fs.constants.R_OK);
    } catch {
      return this.error(`The source file is not readable. Given: ${options.source}`);
    }

    if (userProvidedType) {
      // Process only the type provided by the user.
      try {
        const results = JSON.parse(hactool.run(...[...parameters, '--intype', inputType, options.source]));

        return results;
      } catch (error) {
        // Convert Napi::Error exceptions.
        return this.error(error.message);
      }
    } else {
      let results = {
        warnings: [],
      };

      const sourceExtension = path.extname(options.source).replace('.', '').toLowerCase();
      let orderedTypes = [];
      let priorityType = '';

      // We don't know the input file type because the user didn't provide the type.
      // Guess the input type using the source file extension then put that input type
      // on the top of the inputTypes list so we check that one first.
      if (inputTypes[0] !== sourceExtension) {
        if (['nsp', 'nsz'].includes(sourceExtension)) {
          // This is most likely a PFS0 format file. Try that input type first.
          priorityType = 'pfs0';
        } else if (sourceExtension === 'xci') {
          priorityType = 'xci';
        }

        if (priorityType !== '') {
          orderedTypes = [priorityType, ...inputTypes.filter((t) => t !== priorityType)];
        }
      }

      // The user did not specify an input type so try each type until we do not receive a type error.
      orderedTypes.some((type) => {
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
          const warnings = [...results.warnings, ...check.warnings];

          results = check;
          results.warnings = warnings;
          results.detectedType = type;

          // Break out of the some loop.
          return true;
        }
      });

      return results;
    }
  },
  information: function(options) {
    const parameters = [
      'hactool',
      '--info',
    ];

    return this.run(options, parameters);
  },
  extract: function(options) {
    // Make sure that the user provided a source file to process.
    if (typeof options?.outputDirectory === 'undefined') {
      return this.error('Provide a full path to an output directory using the "outputDirectory " option.');
    }

    if (typeof options.outputDirectory !== 'string') {
      return this.error('The path of the output directory must be a string.');
    }

    // Make sure that the user provided the file name of the file they want to extract.
    if (typeof options?.fileName === 'undefined') {
      return this.error('Provide the name of the file you want to extract using the "fileName" option.');
    }

    if (typeof options.fileName !== 'string') {
      return this.error('The file name of the file you want to extract must be a string.');
    }

    // Make sure that the output directory is writable.
    try {
      fs.accessSync(options.outputDirectory, fs.constants.W_OK);
    } catch {
      return this.error(`The output directory is not writable. Given: ${options.outputDirectory}`);
    }

    const parameters = [
      'hactool',
      '--file',
      options.fileName,
      '--outdir',
      options.outputDirectory,
    ];

    return this.run(options, parameters);
  },
};

module.exports = nodeHactool;

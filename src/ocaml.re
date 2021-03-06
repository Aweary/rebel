/*
 * vim: set ft=rust:
 * vim: set ft=reason:
 */
open Utils;

open Core.Std;

let module Dep = Jenga_lib.Api.Dep;

let module Path = Jenga_lib.Api.Path;

let module Glob = Jenga_lib.Api.Glob;

let module Rule = Jenga_lib.Api.Rule;

let module Action = Jenga_lib.Api.Action;

let module Scheme = Jenga_lib.Api.Scheme;

let binaryOutput = rel dir::(rel dir::buildDirRoot (tsl topLibName)) "app.out";

let jsOutput = rel dir::(rel dir::buildDirRoot (tsl topLibName)) "app.js";

/* We do this way because jsoo needs byte code generated by ocamlc artifacts. */
let compiler = rebelConfig.backend == "native" ? "ocamlopt" : "ocamlc";

/* impl artifact extension  */
let cmox = rebelConfig.backend == "native" ? ".cmx" : ".cmo";

/* library artifact extension  */
let cmax = rebelConfig.backend == "native" ? ".cmxa" : ".cma";


/**
  When we build src folder of any package, we convert the package name in camelCase and
  use it for modules alias. For the top level src, we simply module alias it with `Src`.

  Note: Libname is just last path component of buildDir here
 **/
let moduleAliasFile buildDir::buildDir libName::libName ext::ext =>
  rel dir::buildDir (tsm (libToModule libName) ^ ext);

/*
 We treat each folder as a module/library.

 The module alias file takes the current library foo's first-party sources, e.g. A.re, C/B.re,
 and turn them into a Foo.ml/Foo_Foo.ml file whose content is:

 module A = Foo__A;
 module B = Foo__B;

 We'll then compile this file into foo.cmi/cmo/cmt, and have it opened by default when compiling A.re and
 B (into Foo__A and Foo__B respectively) later. The effect is that, inside A.re, we can refer to B instead
 of Foo__B thanks to the pre-opened Foo.ml/Foo_Foo.ml. But when these files are used by other libraries (which aren't
 compiled with foo.re pre-opened of course), they won't see module A or C, only Foo__A and Foo_C, aka in
 practice, they simply won't see them. This effectively means we've implemented namespacing!

 Note that we're generating a ml file rather than re, because this rebel theoretically works on pure
 ocaml projects too, with no dep on reason. */
let moduleAliasLibScheme
    buildDir::buildDir
    libRoot::libRoot
    libName::libName
    sourcePaths::sourcePaths => {
  let moduleAliasPath ext => moduleAliasFile buildDir::buildDir libName::libName ext::ext;

  /** We omit interface to create the alias file **/
  let sourceNotInterfacePaths = List.filter sourcePaths f::(fun path => not (isInterface path));
  let fileContent =
    List.map
      sourceNotInterfacePaths
      f::(
        fun path =>
          Printf.sprintf
            "module %s = %s\n"
            (tsm (pathToModule path))
            (namespacedName libName::libName path::path)
      ) |>
    String.concat sep::"";
  let sourcePath = moduleAliasPath ".ml";
  let action =
    bashf
      /* We suppress a few warnings here through -w.
         - 49: Absent cmi file when looking up module alias. Aka Foo__A and Foo__B's compiled cmis
         can't be found at the moment this module alias file is compiled. This is normal, since the
         module alias file is the first thing that's compiled (so that we can open it during
         compilation of A.re and B.re into Foo__A and Foo__B). Think of this as forward declaration.

         - 30: Two labels or constructors of the same name are defined in two mutually recursive
         types. I forgot...

         - 40: Constructor or label name used out of scope. I forgot too. Great comment huh?

         More flags:
         -pp refmt option makes ocamlc take our reason syntax source code and pass it through our
         refmt parser first, before operating on the AST.

         -bin-annot: generates cmt files that contains info such as types and bindings, for use with
         Merlin.

         -g: add debugging info. You don't really ever compile without this flag.

         -impl: source file. This flag's needed if the source extension isn't ml. I think.

         -o: output name
         */
      "%s -bin-annot -g -no-alias-deps -w -49 -w -30 -w -40 -c -impl %s -o %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
      compiler
      (tsp sourcePath)
      (tsp (moduleAliasPath cmox));

  /** TODO: do we even need the cmo file here? */
  let targets = List.map f::moduleAliasPath [cmox, ".cmi", ".cmt"];

  /** The Dep's here trigger the compilation of thirdParty npm packages and it is enough
      just specify the thirdParty's top level cma artifact. Also since module alias is dep
      for source file in the libDir so no need to redeclare this dep in the compileSourcesScheme **/
  let thirdPartyNpmLibs = NpmDep.getThirdPartyNpmLibs libDir::libRoot;
  let thirdPartiesCmasDep =
    List.map
      thirdPartyNpmLibs
      f::(
        fun libName => {
          let buildDir = rel dir::buildDirRoot (tsl libName);
          Dep.path (moduleAliasFile buildDir::buildDir libName::libName ext::cmax)
        }
      );
  let compileRule =
    Rule.create
      targets::targets
      (Dep.all [Dep.path sourcePath, ...thirdPartiesCmasDep] |> mapD (fun _ => action));
  let contentRule =
    Rule.create targets::[sourcePath] (Dep.return (Action.save fileContent target::sourcePath));
  Scheme.rules [contentRule, compileRule]
};

/* We compile each file in the current library (say, foo). If a file's Bar.re, it'll be compiled to
   foo__Bar.{cmi, cmo, cmt}. As to why we're namespacing compiled outputs like this, see
   `moduleAliasFileScheme`. */
let compileSourcesScheme
    libDir::libDir
    libRoot::libRoot
    buildDir::buildDir
    libName::libName
    sourcePaths::sourcePaths => {
  /* This is the module alias file generated through `moduleAliasFileScheme`, that we said
     we're gonna `-open` during `ocamlc` */
  let moduleAliasPath ext => moduleAliasFile buildDir::buildDir libName::libName ext::ext;
  let moduleName = Path.basename @@ moduleAliasPath "";

  /** Compute Module Alias dependencies for dependencies only */
  let moduleAliasDep = Dep.all_unit (
    List.map [cmox, ".cmi", ".cmt", ".ml"] f::(fun ext => ext |> moduleAliasPath |> Dep.path)
  );

  /** Get npm dependencies and ocamlfind dependencies from the package's package.json **/
  let thirdPartyNpmLibs = NpmDep.getThirdPartyNpmLibs libDir::libRoot;
  let thirdPartyOcamlfindLibNames = NpmDep.getThirdPartyOcamlfindLibs libDir::libRoot;

  /** Include all ocamlfind dependencies under -package flag **/
  let ocamlfindPackagesStr =
    switch thirdPartyOcamlfindLibNames {
    | [] => ""
    | libs => "-package " ^ (libs |> List.map f::tsl |> String.concat sep::",")
    };

  /** Need to include all the third party build directories for cmo artifacts. We compute these
      parts from the source paths instead of _build directory paths because on a fresh these directories
      will not be present in _build. Hence the indirect route. **/
  let thirdPartyNpmDirPaths =
    List.map thirdPartyNpmLibs f::(fun libName => rel dir::buildDirRoot (tsl libName));

  /** flag to include all the dependencies build dir's **/
  let includeDir =
    List.map thirdPartyNpmDirPaths f::(fun path => "-I " ^ tsp path) |> String.concat sep::" ";

  /** Hard Coded Rules for special packages */
  let extraFlags =
    if (List.mem thirdPartyOcamlfindLibNames (Lib "core")) {
      "-thread -package threads"
    } else {
      ""
    };

  /** Compute build graph (targets, dependencies) for the current path */
  let compileEachSourcePath path =>
    OcamlDep.ocamlDepCurrentSources sourcePath::path paths::sourcePaths |>
    mapD (
      fun firstPartyDeps => {
        let isInterface' = isInterface path;
        let hasInterface' = hasInterface sourcePaths::sourcePaths path;

        /** Helper functions to generate build dir paths **/
        let namespacedPath ext =>
          rel dir::buildDir (namespacedName libName::libName path::path ^ ext);

        /** Debug Info */
        /* print_endline ("Path: " ^ tsp path);
           print_endline "First Party Deps: ";
           print_endline ("Had Interface: " ^ string_of_bool hasInterface');
           print_endline ("Build Dir: " ^ tsp buildDir);
           print_endline ("Lib Dir: " ^ tsp libDir); */

        /** Rule for compiling .re/rei/ml/mli to .cmo **/
        /*
         Most of the flags here have been explained previously in `moduleAliasFileScheme`.
         -intf-suffix: tells ocamlc what the interface file's extension is.

         -c: compile only, don't link yet.
         Example command: ocamlc -pp refmt -bin-annot -g -w -30 -w -40 -open Foo -I \
         path/to/js_of_ocaml path/to/js_of_ocaml/js_of_ocaml.cma -I ./ -I ../fooDependsOnMe -I \
         ../fooDependsOnMeToo -o foo__CurrentSourcePath -intf-suffix .rei -c -impl \
         path/to/CurrentSourcePath.re */
        let action =
          bashf
            (
              if isInterface' {
                "ocamlfind %s -pp refmt -g -w -30 -w -40 %s -I %s %s %s %s -o %s -c -intf %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
              } else if (
                hasInterface' && String.is_suffix (Path.basename path) suffix::".re"
              ) {
                "ocamlfind %s -pp refmt -bin-annot -g -w -30 -w -40 %s -I %s %s %s %s -o %s -c -intf-suffix .rei -impl %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
              } else {
                "ocamlfind %s -pp refmt -bin-annot -g -w -30 -w -40 %s -I %s %s %s %s -o %s -c -impl %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
              }
            )
            compiler
            ("-open " ^ moduleName)
            (tsp buildDir)
            extraFlags
            ocamlfindPackagesStr
            includeDir
            (tsp (namespacedPath ""))
            (tsp path);

        /** compiling here only needs cmis. If the interface signature doesn't change, ocaml doesn't need
            to recompile the dependent modules. Win. */
        let firstPartyArtifactDeps =
          sourcePaths |>
          List.filter
            f::(fun path => List.exists firstPartyDeps f::(fun m => m == pathToModule path)) |>
          List.map
            f::(
              fun path => relD dir::buildDir (namespacedName libName::libName path::path ^ ".cmi")
            );
        let firstPartyArtifactDeps =
          if (not isInterface' && hasInterface') {
            [
              /* We're a source file with an interface; include our own cmi as a dependency (our interface
                 file should be compile before ourselves). */
              relD dir::buildDir (namespacedName libName::libName path::path ^ ".cmi"),
              ...firstPartyArtifactDeps
            ]
          } else {
            firstPartyArtifactDeps
          };

        /** The overall dependencies include the js artifacts of the both self and third party
            and interface artifact if an interface exits **/
        let deps = Dep.all_unit [Dep.path path, moduleAliasDep, ...firstPartyArtifactDeps];

        /** Compute the artifacts extensions generate for each file type and then generate the
            correct build dir path for them **/
        let targets = {
          let extns =
            if isInterface' {
              [".cmi"]
            } else if hasInterface' {
              [cmox, ".cmt"]
            } else {
              [".cmi", cmox, ".cmt"]
            };
          List.map f::namespacedPath extns
        };
        Rule.create targets::targets (Dep.map deps (fun () => action))
      }
    );
  Scheme.rules_dep (Dep.all (List.map sourcePaths f::compileEachSourcePath))
};

/* This function assumes we're not using it at the top level src directory.
   Cma is a library file for the current library which bundles up all the lib's first-party compiled sources.
   This way, it's much easier, at the end, at the top level, to include each library's cma file to compile the
   final executable, than to tediously pass every single source file from every lib in order.

   There's a caveat though. We said we're only bundling up the current lib's first-party code; We _could_ have
   bundled up its third-party deps' cma too, but then we might get into duplicate artifact problem caused by
   e.g. library A and B both requiring and bundling C. So we can only bundle first-party code, and then, at
   the top, take all the transitive dependencies (libraries) cmas, figure out their relative order, and pass
   them in that order to the ocamlc command (this logic is in `finalOutputsScheme` below). Still tedious, but
   at least we're not passing individual source files in order. */
let compileCmaScheme sortedSourcePaths::sortedSourcePaths libName::libName buildDir::buildDir => {
  let moduleAliasPath ext => moduleAliasFile buildDir::buildDir libName::libName ext::ext;
  let moduleName = tsm (libToModule libName);
  let (moduleAliasCmaPath, moduleAliasCmoPath) = (moduleAliasPath cmax, moduleAliasPath cmox);

  /** All the cmos artifacts from the files in the libDir **/
  let cmos =
    List.map
      /* To compile one cma file, we need to pass the compiled first-party sources in order to ocamlc */
      sortedSourcePaths
      f::(fun path => rel dir::buildDir (namespacedName libName::libName path::path ^ cmox));
  let cmosString = List.map cmos f::tsp |> String.concat sep::" ";

  /** Final bundling. Time to get all the transitive dependencies... */
  Scheme.rules [
    Rule.simple
      targets::[moduleAliasCmaPath]
      deps::(List.map [moduleAliasCmoPath, ...cmos] f::Dep.path)
      action::(
        bashf
          /* Flags:
             -open: compile the file as if [file being opened] was opened at the top of the file. In
             our case, we open our module alias file generated with `moduleAliasFileScheme`. See that
             function for more comment.

             -a: flag for building a library.

             -o: output file name.
             */
          /* Example command: ocamlc -g -open Foo -a -o lib.cma foo.cmo aDependsOnMe.cmo a.cmo b.cmo */
          "%s -g -open %s -a -o %s %s %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
          compiler
          moduleName
          (tsp moduleAliasCmaPath)
          (tsp moduleAliasCmoPath)
          cmosString
      )
  ]
};

let finalOutputsScheme buildDir::buildDir libName::libName sortedSourcePaths::sortedSourcePaths => {
  let moduleAliasCmoPath = rel dir::buildDir (tsm (libToModule topLibName) ^ cmox);

  /** All the cmos artifacts from the files in the toplevel src dir **/
  let cmos =
    List.map
      /* To compile one cma file, we need to pass the compiled first-party sources in order to ocamlc */
      sortedSourcePaths
      f::(fun path => rel dir::buildDir (namespacedName libName::libName path::path ^ cmox));
  let cmosString = List.map cmos f::tsp |> String.concat sep::" ";

  /** Gather all the cma artifacts compiled from npm package **/
  let transitiveCmaPaths =
    List.map
      NpmDep.sortedTransitiveThirdPartyNpmLibsIncludingSelf's
      f::(
        fun libName =>
          rel dir::(rel dir::buildDirRoot (tsl libName)) (tsm (libToModule libName) ^ cmax)
      );
  let ocamlfindPackagesStr =
    if (NpmDep.transitiveThirdPartyOcamlfindLibsIncludingSelf's == []) {
      ""
    } else {
      "-linkpkg -package " ^ (
        List.map NpmDep.transitiveThirdPartyOcamlfindLibsIncludingSelf's f::tsl |>
        String.concat sep::","
      )
    };

  /** Hard Coded Rules for special packages */
  /* TODO add ocamlcFlags */
  let extraFlags =
    if (List.mem NpmDep.transitiveThirdPartyOcamlfindLibsIncludingSelf's (Lib "core")) {
      "-thread -package threads"
    } else {
      ""
    };
  let action =
    bashf
      /* For ease of coding, we'll blindly include js_of_ocaml in the -I search path here, in case
         the module invokes some jsoo's Js module-related stuff. */
      /* Example command: ocamlc -g -I path/to/js_of_ocaml path/to/js_of_ocaml/js_of_ocaml.cma \
         -open Top -o app.out  ../barDependsOnMe/lib.cma ../bar/lib.cma ../baz/lib.cma \
         top.cmo aDependsOnMe.cmo a.cmo moreFirstPartyCmo.cmo */
      /* Flags:
         -I: search path(s), when ocamlc looks for modules referenced inside the file.

         -open: compile the file as if [file being opened] was opened at the top of the file. In
         our case, we open our module alias file generated with `moduleAliasFileScheme`. See that
         function for more comment.

         -o: output file name.
         */
      "ocamlfind %s %s %s -g -open %s -o %s %s %s %s 2>&1| berror; (exit ${PIPESTATUS[0]})"
      compiler
      extraFlags
      ocamlfindPackagesStr
      (tsm (libToModule libName))
      (tsp binaryOutput)
      (transitiveCmaPaths |> List.map f::tsp |> String.concat sep::" ")
      (tsp moduleAliasCmoPath)
      cmosString;
  let backend = rebelConfig.backend;
  let nativeRule =
    /* We check here for jsoo because jsoo needs binaryOutput */
    backend == "byte" || backend == "native" || backend == "jsoo" ?
      [
        Rule.simple
          targets::[binaryOutput]
          deps::(
            /* TODO: I don't think cmis and cmts are being read here, so we don't need to include them. */
            cmos @ transitiveCmaPaths |> List.map f::Dep.path
          )
          action::action
      ] :
      [];
  let javascriptRule =
    backend == "jsoo" ?
      [
        Rule.simple
          targets::[jsOutput]
          deps::[Dep.path binaryOutput]
          action::(
            bashf
              /* I don't know what the --linkall flag does, and does the --pretty flag work? Because the
                 output is still butt ugly. Just kidding I love you guys. */
              "js_of_ocaml --source-map --no-inline --debug-info --pretty --linkall -o %s %s"
              (tsp jsOutput)
              (tsp binaryOutput)
          )
      ] :
      [];
  Scheme.rules (List.append nativeRule javascriptRule)
};

let compileLibScheme
    libDir::libDir
    libRoot::libRoot
    libName::libName
    isTopLevelLib::isTopLevelLib
    buildDir::buildDir =>
  Dep.all (getSourceFiles dir::libDir |> List.map f::Dep.return) |>
  bindD (
    fun unsortedPaths =>
      /* compute all the subdirectories */
      /** Construct schemes for modules alias, source files and cma artifact/final executable **/
      OcamlDep.sortPathsTopologically paths::unsortedPaths |>
      mapD (
        fun sortedPaths => Scheme.all [
          moduleAliasLibScheme
            buildDir::buildDir libRoot::libRoot libName::libName sourcePaths::unsortedPaths,
          compileSourcesScheme
            libDir::libDir
            libRoot::libRoot
            buildDir::buildDir
            libName::libName
            sourcePaths::unsortedPaths,
          isTopLevelLib && tsp libDir == "src" ?
            /* if we're at the final, top level compilation, there's no need to build a cma output (and
               then generate an executable from it). We can cut straight to generating the executable. See
               `finalOutputsScheme`. */
            finalOutputsScheme buildDir::buildDir libName::libName sortedSourcePaths::sortedPaths :
            compileCmaScheme buildDir::buildDir libName::libName sortedSourcePaths::sortedPaths
        ]
      )
  ) |> Scheme.dep;

let scheme dir::dir =>
  if (dir == Path.the_root) {
    let defaultRule =
      switch rebelConfig.backend {
      | "jsoo" => [Dep.path jsOutput]
      | "byte"
      | "native" => [Dep.path binaryOutput]
      | _ => []
      };
    Scheme.rules [Rule.default dir::dir defaultRule]
  } else if (
    Path.is_descendant dir::buildDirRoot dir
  ) {
    let dirName = Path.basename dir;
    let libName = Lib dirName;
    let libDir = convertBuildDirToLibDir buildDir::dir;
    let packageName = extractPackageName dir::dir;
    let isTopLevelLib = packageName == "src";
    let libRoot = isTopLevelLib ? Path.dirname topSrcDir : rel dir::nodeModulesRoot packageName;

    /** start compile */
    compileLibScheme
      libDir::libDir libRoot::libRoot libName::libName isTopLevelLib::isTopLevelLib buildDir::dir
  } else {
    Scheme.no_rules
  };

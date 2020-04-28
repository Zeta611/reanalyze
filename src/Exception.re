module Event = {
  type kind =
    | Raises
    | Catches
    | Calls
    | Lib;

  type t = {
    kind,
    loc: Location.t,
  };
};

let valueBindingsTable = Hashtbl.create(15);

let raisesLibTable = {
  let table = Hashtbl.create(15);
  [
    "List.hd",
    "List.tl",
    "List.nth",
    "List.nth_opt",
    "List.init",
    "List.iter2",
    "List.map2",
    "List.fold_left2",
    "List.fold_right2",
    "List.for_all2",
    "List.exists2",
    "List.find",
    "List.assoc",
    "List.combine",
  ]
  |> List.iter(s => Hashtbl.add(table, s, ()));
  table;
};

let traverseAst = {
  let super = Tast_mapper.default;

  let currentId = ref("");
  let currentEvents = ref([]);

  let expr = (self: Tast_mapper.mapper, e: Typedtree.expression) => {
    switch (e.exp_desc) {
    | Texp_apply({exp_desc: Texp_ident(callee, _, _)}, _) =>
      let functionName = Path.name(callee);
      if (functionName == "Pervasives.raise") {
        currentEvents :=
          [{Event.kind: Raises, loc: e.exp_loc}, ...currentEvents^];
      } else {
        switch (Hashtbl.find_opt(valueBindingsTable, functionName)) {
        | Some((loc, Some(_))) =>
          currentEvents := [{Event.kind: Calls, loc}, ...currentEvents^]
        | _ =>
          if (Hashtbl.mem(raisesLibTable, functionName)) {
            currentEvents :=
              [{Event.kind: Lib, loc: e.exp_loc}, ...currentEvents^];
          }
        };
      };
    | Texp_match(_) when e.exp_desc |> Compat.texpMatchHasExceptions =>
      currentEvents :=
        [{Event.kind: Catches, loc: e.exp_loc}, ...currentEvents^]
    | Texp_try(_) =>
      currentEvents :=
        [{Event.kind: Catches, loc: e.exp_loc}, ...currentEvents^]
    | _ => ()
    };
    super.expr(self, e);
  };

  let value_binding = (self: Tast_mapper.mapper, vb: Typedtree.value_binding) => {
    let oldId = currentId^;
    let oldEvents = currentEvents^;
    switch (vb.vb_pat.pat_desc) {
    | Tpat_var(id, {loc}) =>
      currentId := Ident.name(id);
      let hasRaisesAnnotation =
        vb.vb_attributes |> Annotation.getAttributePayload((==)("raises"));
      Hashtbl.replace(
        valueBindingsTable,
        Ident.name(id),
        (loc, hasRaisesAnnotation),
      );
    | _ => ()
    };
    let res = super.value_binding(self, vb);
    let eventIsCatches = (event: Event.t) => event.kind == Catches;
    let (eventsCatches, eventsNotCatches) =
      currentEvents^ |> List.partition(event => eventIsCatches(event));
    let hasRaisesAnnotation =
      switch (Hashtbl.find_opt(valueBindingsTable, currentId^)) {
      | Some((_loc, Some(_))) => true
      | _ => false
      };
    let shouldReport =
      eventsNotCatches != [] && eventsCatches == [] && !hasRaisesAnnotation;
    if (shouldReport) {
      Log_.info(
        ~loc=(eventsNotCatches |> List.hd).loc,
        ~name="Exception Analysis",
        (ppf, ()) =>
        Format.fprintf(
          ppf,
          "%s might raise an exception and is not annotated with @raises",
          currentId^,
        )
      );
    };
    currentId := oldId;
    currentEvents := oldEvents;
    res;
  };

  Tast_mapper.{...super, expr, value_binding};
};

let processStructure = (structure: Typedtree.structure) => {
  structure |> traverseAst.structure(traverseAst) |> ignore;
};

let processCmt = (cmt_infos: Cmt_format.cmt_infos) =>
  switch (cmt_infos.cmt_annots) {
  | Interface(_) => ()
  | Implementation(structure) => processStructure(structure)
  | _ => ()
  };

let reportResults = (~ppf) =>
  Format.fprintf(ppf, "Report Exception results@.");
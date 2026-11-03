Building and documentation
==========================

Everything goes through the ``ci`` script at the repository root (``./ci`` in Git Bash,
``.\ci.cmd`` in PowerShell or cmd).

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Command
     - Action
   * - ``./ci build_sketch``
     - Compile the firmware with arduino-cli (downloads it and the UNO R4 core if missing).
   * - ``./ci build_KSPSerialIO``
     - Compile the plugin with msbuild, xbuild or dotnet.
   * - ``./ci build_all``
     - Both of the above.
   * - ``./ci install``
     - Copy the plugin and ``config.xml`` to ``$KSPDIR/GameData/KSPSerialIO``.
   * - ``./ci docs``
     - Doxygen API reference into ``build/docs/html``.
   * - ``./ci diagrams``
     - Render every PlantUML diagram to SVG in ``build/diagrams``.
   * - ``./ci sphinx``
     - Build this site into ``build/sphinx`` (includes the Doxygen output under ``api/``).
   * - ``./ci clean``
     - Remove build output.

Requirements for the documentation
----------------------------------

* **Python 3** with ``pip`` (a virtual environment is created in ``build/venv``).
* **Java** to run PlantUML. ``plantuml.jar`` is downloaded to ``build/tools`` unless the
  ``PLANTUML_JAR`` environment variable points to one.
* **Doxygen** for the API reference.
* Python packages are listed in ``docs/requirements.txt``.

Diagram conventions
-------------------

Diagrams are PlantUML sources in ``docs/diagrams``, one per file, and use standard UML notation:
class, sequence, state machine, activity and deployment diagrams. They are included with the
``uml`` directive, so editing a ``.puml`` file and rebuilding is all that is needed. Class
diagrams use PlantUML's built-in layout engine, so Graphviz is not required.

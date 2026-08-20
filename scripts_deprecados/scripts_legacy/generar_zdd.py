import graphviz

def crear_mtzdd_final():
    dot = graphviz.Digraph('MTZDD_Final', format='png')
    dot.attr(rankdir='TB', compound='true', nodesep='0.8', ranksep='0.8')
    dot.attr('node', style='filled', fontname='Helvetica')

    estilo_mtzdd = {'shape': 'ellipse', 'fillcolor': '#D0E4F5', 'color': '#2B7CE9', 'penwidth': '2', 'fontname': 'Helvetica-Bold'}
    estilo_zdd = {'shape': 'circle', 'fillcolor': '#E2F0D9', 'color': '#548235', 'penwidth': '2'}
    estilo_terminal = {'shape': 'box', 'fillcolor': '#F2F2F2', 'color': '#7F7F7F', 'fontname': 'Helvetica-Bold'}
    estilo_raiz = {'shape': 'cds', 'fillcolor': '#E1D5E7', 'color': '#9673A6', 'fontname': 'Helvetica-Bold'}
    estilo_puntero = {'shape': 'box', 'fillcolor': '#FFF2CC', 'color': '#D6B656', 'style': 'filled,rounded', 'penwidth': '2', 'fontname': 'Helvetica-Bold'}

    # ================= PISO 1: MTZDD (Mapeo a Punteros) =================
    dot.node('TITLE_1', 'PISO 1: MTZDD Global\nNodos = Documentos Maestros | Terminales = Punteros', 
             shape='plaintext', fillcolor='white', fontcolor='blue', fontsize='14', fontname='Helvetica-Bold')
    
    # Raíces
    dot.node('VOC_HOLA', 'Raíz: "hola"', **estilo_raiz)
    dot.node('VOC_CHAO', 'Raíz: "chao"', **estilo_raiz)

    # Nodos de Decisión (Documentos)
    dot.node('DOC2_H', 'Doc 2', **estilo_mtzdd)
    dot.node('DOC1_H', 'Doc 1', **estilo_mtzdd)
    dot.node('DOC2_C', 'Doc 2', **estilo_mtzdd)

    # Terminales (Los Punteros y el 0)
    dot.node('PTR_A', 'Puntero A\n(Patrón {1,2})', **estilo_puntero)
    dot.node('PTR_B', 'Puntero B\n(Patrón {1,3})', **estilo_puntero)
    dot.node('0_MT', '0\n(No encontrado)', **estilo_terminal)

    # Conexiones "hola" -> [(1,A), (2,B)]
    dot.edge('VOC_HOLA', 'DOC2_H', color='#9673A6', penwidth='2')
    dot.edge('DOC2_H', 'PTR_B', color='red', penwidth='2', label=' Alta (Es el Doc 2)')
    dot.edge('DOC2_H', 'DOC1_H', style='dashed', color='gray', label=' Baja (Buscar otro)')
    dot.edge('DOC1_H', 'PTR_A', color='red', penwidth='2', label=' Alta (Es el Doc 1)')
    dot.edge('DOC1_H', '0_MT', style='dashed', color='gray', label=' Baja')

    # Conexiones "chao" -> [(2,A)]
    dot.edge('VOC_CHAO', 'DOC2_C', color='#9673A6', penwidth='2')
    dot.edge('DOC2_C', 'PTR_A', color='red', penwidth='2', label=' Alta (Es el Doc 2)')
    dot.edge('DOC2_C', '0_MT', style='dashed', color='gray', label=' Baja')

    # ================= PISO 2: ZDD POOL TEMPORAL =================
    dot.node('TITLE_2', 'PISO 2: Bosque Compartido ZDD\nVariables = Versiones', 
             shape='plaintext', fillcolor='white', fontcolor='green', fontsize='14', fontname='Helvetica-Bold')

    dot.node('v1_A', 'v1', **estilo_zdd)
    dot.node('v2_A', 'v2', **estilo_zdd)
    dot.node('v1_B', 'v1', **estilo_zdd)
    dot.node('v3_B', 'v3', **estilo_zdd)
    dot.node('1_ZDD', '1 (Aceptación)', **estilo_terminal)
    dot.node('0_ZDD', '0', **estilo_terminal)

    # Conectar Punteros al ZDD
    dot.edge('PTR_A', 'v1_A', color='orange', penwidth='3', style='dotted', label=' Extrae')
    dot.edge('PTR_B', 'v1_B', color='orange', penwidth='3', style='dotted', label=' Extrae')

    # Trazado Patrón A {1,2}
    dot.edge('v1_A', '0_ZDD', style='dashed', color='gray')
    dot.edge('v1_A', 'v2_A', color='blue', penwidth='2')
    dot.edge('v2_A', '0_ZDD', style='dashed', color='gray')
    dot.edge('v2_A', '1_ZDD', color='blue', penwidth='2')

    # Trazado Patrón B {1,3}
    dot.edge('v1_B', '0_ZDD', style='dashed', color='gray')
    dot.edge('v1_B', 'v3_B', color='blue', penwidth='2')
    dot.edge('v3_B', '0_ZDD', style='dashed', color='gray')
    dot.edge('v3_B', '1_ZDD', color='blue', penwidth='2')

    dot.render('mtzdd_terminales_punteros', view=False)
    print("Grafo generado exitosamente: mtzdd_terminales_punteros.png")

if __name__ == '__main__':
    crear_mtzdd_final()
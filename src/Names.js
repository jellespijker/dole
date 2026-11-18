.pragma library

// The kit's sounds by id, in the user's language.
function kit(id) {
    switch (id) {
    case "boom": return qsTr("Boom")
    case "knock": return qsTr("Knock")
    case "crack": return qsTr("Crack")
    case "dusty": return qsTr("Dusty")
    case "rim": return qsTr("Rim")
    case "clap": return qsTr("Clap")
    case "hat": return qsTr("Hat")
    case "openhat": return qsTr("Open hat")
    case "shaker": return qsTr("Shaker")
    case "tom": return qsTr("Tom")
    case "crash": return qsTr("Crash")
    case "clang": return qsTr("Clang")
    case "stab": return qsTr("Stab")
    case "bell": return qsTr("Bell")
    case "bass": return qsTr("Bass")
    case "horn": return qsTr("Horn")
    }
    return id
}

// What a pad plays: its sample's name, or its kit sound's.
function pad(p) {
    return p.sample !== "" ? p.sample : kit(p.id)
}

// The keys that play the pads, by pad: the bottom row is pad 1, as on an MPC.
const keys = ["Z", "X", "C", "V", "A", "S", "D", "F", "Q", "W", "E", "R", "1", "2", "3", "4"]

function grit(g) {
    return g === 1 ? qsTr("12-bit") : g === 2 ? qsTr("Crushed") : qsTr("Clean")
}
